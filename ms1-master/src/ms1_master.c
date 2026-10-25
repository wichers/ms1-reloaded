/**
 * @file ms1_master.c
 * @brief M-CAN Master implementation.
 *
 * Implements the master side of the M-CAN protocol for controlling MS1 slaves.
 * Based on CAN_Doku_V101_de.pdf (ECoS/MS1/CS1 CAN protocol documentation).
 */

#include "ms1_master.h"
#include <string.h>

/* ========================================================================
 * Internal helpers - forward declarations
 * ====================================================================== */

static void master_send_frame(const ms1_master_t *m, uint32_t id, uint8_t dlc,
                              const uint8_t *data);
static void master_send_bus_occupied(ms1_master_t *m);

static void master_handle_registration(ms1_master_t *m, const ms1_can_frame_t *frame);
static void master_handle_operational(ms1_master_t *m, const ms1_can_frame_t *frame);
static void master_handle_flash(ms1_master_t *m, const ms1_can_frame_t *frame);

static void master_handle_setup_frame(ms1_master_t *m, ms1_slave_ctx_t *slave,
                                      uint8_t slave_id, const ms1_op_id_t *oid,
                                      const ms1_can_frame_t *frame);
static void master_handle_control_frame(ms1_master_t *m, ms1_slave_ctx_t *slave,
                                        uint8_t slave_id, const ms1_op_id_t *oid,
                                        const ms1_can_frame_t *frame);
static void master_handle_heartbeat_frame(ms1_master_t *m, ms1_slave_ctx_t *slave,
                                          uint8_t slave_id, const ms1_op_id_t *oid,
                                          const ms1_can_frame_t *frame);
static void master_handle_assignment_frame(ms1_master_t *m, ms1_slave_ctx_t *slave,
                                           uint8_t slave_id, const ms1_op_id_t *oid,
                                           const ms1_can_frame_t *frame);
static void master_handle_stop_go_frame(ms1_master_t *m, ms1_slave_ctx_t *slave,
                                        uint8_t slave_id, const ms1_op_id_t *oid,
                                        const ms1_can_frame_t *frame);

static void master_check_heartbeat_timeouts(ms1_master_t *m);
static ms1_slave_ctx_t *master_find_slave_by_node(ms1_master_t *m, uint8_t node,
                                                    uint8_t *slave_id_out);
static ms1_slave_ctx_t *master_find_slave_by_uid(ms1_master_t *m, const uint8_t uid[4],
                                                   uint8_t *slave_id_out);
static ms1_slave_ctx_t *master_alloc_slave(ms1_master_t *m, uint8_t *slave_id_out);
static int8_t master_find_lokstack_by_handle(const ms1_slave_ctx_t *slave, uint16_t handle);

static void master_send_op_frame(const ms1_master_t *m, uint8_t priority,
                                 uint16_t handle, uint8_t command, uint8_t node,
                                 uint8_t dlc, const uint8_t *data);

static void master_send_lokstack_entry(const ms1_master_t *m, ms1_slave_ctx_t *slave,
                                       uint8_t entry_idx);
static void master_send_lok_type(const ms1_master_t *m, ms1_slave_ctx_t *slave,
                                 int8_t lok_slot, uint16_t lok_handle);
static void master_send_lok_format(const ms1_master_t *m, ms1_slave_ctx_t *slave,
                                   int8_t lok_slot, uint16_t lok_handle,
                                   const uint8_t *query_data);
static void master_send_lok_func_def(const ms1_master_t *m, ms1_slave_ctx_t *slave,
                                     int8_t lok_slot, uint16_t lok_handle, uint8_t fkt);
static void master_send_lok_func_value(const ms1_master_t *m, ms1_slave_ctx_t *slave,
                                       int8_t lok_slot, uint16_t lok_handle, uint8_t fkt);
static void master_send_lok_name_segment(const ms1_master_t *m, ms1_slave_ctx_t *slave,
                                         int8_t lok_slot, uint16_t lok_handle,
                                         uint8_t offset);
static void master_send_lok_assignment(const ms1_master_t *m, ms1_slave_ctx_t *slave,
                                       int8_t lok_slot, uint16_t lok_handle);

static void master_broadcast_status(ms1_master_t *m, uint8_t status_idx, uint8_t value);

/* ========================================================================
 * Initialization
 * ====================================================================== */

void ms1_master_init(ms1_master_t *master,
                      const ms1_platform_t *platform,
                      const ms1_master_config_t *config,
                      const ms1_master_callbacks_t *callbacks)
{
    memset(master, 0, sizeof(*master));
    master->platform = platform;
    master->config = *config;
    if (callbacks) {
        master->callbacks = *callbacks;
    }

    /* Initialize slave slots */
    for (int i = 0; i < MS1_MAX_SLAVES; i++) {
        master->slaves[i].state = MS1_SLAVE_IDLE;
        memset(master->slaves[i].lokstack, -1, sizeof(master->slaves[i].lokstack));
        master->slaves[i].selected_lok = -1;
    }

    /* Initialize lok database */
    for (int i = 0; i < MS1_MAX_LOKS; i++) {
        master->loks[i].active = false;
    }

    master->next_node_pair = 0;
    master->next_flash_handle = 0x02;
    master->next_lok_handle = 0x0001;
    master->bus_occupied_last_ms = 0;
    master->bus_occupied_burst_remaining = MS1_BUS_OCCUPIED_BURST_COUNT;
}

/* ========================================================================
 * Main poll loop
 * ====================================================================== */

void ms1_master_poll(ms1_master_t *master)
{
    uint32_t now = master->platform->millis();

    /* --- Bus-occupied frame burst --- */
    if (master->bus_occupied_burst_remaining > 0) {
        master_send_bus_occupied(master);
        master->bus_occupied_burst_remaining--;
        if (master->bus_occupied_burst_remaining == 0) {
            master->bus_occupied_last_ms = now;
        }
    } else if ((now - master->bus_occupied_last_ms) >= MS1_BUS_OCCUPIED_INTERVAL_MS) {
        master->bus_occupied_burst_remaining = MS1_BUS_OCCUPIED_BURST_COUNT;
    }

    /* --- Process incoming frames --- */
    ms1_can_frame_t frame;
    while (master->platform->can_receive(&frame)) {
        uint8_t node = frame.id & 0x7Fu;

        if (node == MS1_NODE_REG) {
            /* Registration frame */
            master_handle_registration(master, &frame);
        } else if (node == MS1_NODE_FLASH) {
            /* Flash frame */
            master_handle_flash(master, &frame);
        } else {
            /* Operational frame - find slave by node */
            master_handle_operational(master, &frame);
        }
    }

    /* --- Check heartbeat timeouts --- */
    master_check_heartbeat_timeouts(master);
}

/* ========================================================================
 * Bus-occupied frame
 * ====================================================================== */

static void master_send_bus_occupied(ms1_master_t *m)
{
    uint8_t data[8] = {0};
    master_send_frame(m, MS1_ID_BUS_OCCUPIED, 8, data);
}

/* ========================================================================
 * Frame send helper
 * ====================================================================== */

static void master_send_frame(const ms1_master_t *m, uint32_t id, uint8_t dlc,
                              const uint8_t *data)
{
    ms1_can_frame_t frame;
    frame.id = id;
    frame.dlc = dlc;
    if (data && dlc > 0) {
        memcpy(frame.data, data, dlc);
    } else {
        memset(frame.data, 0, 8);
    }
    m->platform->can_send(&frame);
}

static void master_send_op_frame(const ms1_master_t *m, uint8_t priority,
                                 uint16_t handle, uint8_t command, uint8_t node,
                                 uint8_t dlc, const uint8_t *data)
{
    ms1_op_id_t oid;
    oid.priority = priority;
    oid.handle = handle;
    oid.command = command;
    oid.node = node;
    master_send_frame(m, ms1_id_encode_op(&oid), dlc, data);
}

/* ========================================================================
 * Registration handler
 * ====================================================================== */

static void master_handle_registration(ms1_master_t *m, const ms1_can_frame_t *frame)
{
    ms1_reg_id_t rid;
    ms1_id_decode_reg(frame->id, &rid);

    /* Process registration frames from slaves.
     * The firmware uses the operational CAN ID layout with registration
     * fields packed into the handle: uid_byte(8)|stage(3)|unused(4)|dir(1).
     * Direction: 0=from slave, 1=from master. */
    if (rid.priority != MS1_PRIO_REG_FLASH) {
        return;
    }

    uint8_t stage = rid.stage;

    if (stage == MS1_REG_STAGE_UID3) {
        /* Stage 0: First contact. Allocate or find a slave slot. */
        uint8_t slave_id;
        ms1_slave_ctx_t *slave = master_alloc_slave(m, &slave_id);
        if (!slave) {
            return; /* No free slots */
        }

        /* Reset slave context */
        memset(slave->uid, 0, 4);
        slave->mid = 0;
        slave->state = MS1_SLAVE_REG_STAGE_0;
        slave->uid[0] = rid.uid_byte; /* UID byte 3 (MSB) */
        slave->setup_sd_done = false;
        slave->setup_sy_done = false;
        slave->setup_status_done = false;
        slave->setup_lokstack_done = false;
        slave->lokstack_count = 0;
        slave->selected_lok = -1;
        slave->flash_requested = false;
        memset(slave->lokstack, -1, sizeof(slave->lokstack));

        /* Respond: Master sends with direction=1, same stage, master UID byte */
        ms1_reg_id_t resp;
        resp.priority = MS1_PRIO_REG_FLASH;
        resp.uid_byte = m->config.uid[0]; /* Master's UID byte for this stage */
        resp.stage = stage;
        resp.direction = MS1_DIR_FROM_MASTER;
        resp.node = MS1_NODE_REG;

        uint8_t data[8];
        memset(data, 0, 8);
        /* Data[0..3]: UID[3..0] of master */
        data[0] = m->config.uid[0]; /* UID[3] */
        data[1] = m->config.uid[1]; /* UID[2] */
        data[2] = m->config.uid[2]; /* UID[1] */
        data[3] = m->config.uid[3]; /* UID[0] */
        /* Data[7]: master type indicator */
        data[7] = 0x00; /* MS-Master=0x00, CS/E-Master differ */

        master_send_frame(m, ms1_id_encode_reg(&resp), 8, data);
        slave->state = MS1_SLAVE_REG_STAGE_1;
        return;
    }

    /* For stages 1-7, find the slave that's in the corresponding registration state */
    ms1_slave_ctx_t *slave = NULL;
    uint8_t slave_id = 0;

    for (uint8_t i = 0; i < MS1_MAX_SLAVES; i++) {
        /* Match by expected state */
        if ((int)m->slaves[i].state == (int)(MS1_SLAVE_REG_STAGE_0 + stage)) {
            slave = &m->slaves[i];
            slave_id = i;
            break;
        }
    }
    if (!slave) {
        return;
    }

    /* Store UID byte based on stage (stages 0-3 carry one UID byte each) */
    switch (stage) {
    case MS1_REG_STAGE_UID2:
        slave->uid[1] = rid.uid_byte;
        break;
    case MS1_REG_STAGE_UID1:
        slave->uid[2] = rid.uid_byte;
        break;
    case MS1_REG_STAGE_UID0:
        slave->uid[3] = rid.uid_byte;
        break;
    default:
        break;
    }

    /* Build response — master echoes same stage, with its own UID byte for
     * stages 0-3 (one byte per stage) and uid_byte=0 for stages 4-7. */
    ms1_reg_id_t resp;
    resp.priority = MS1_PRIO_REG_FLASH;
    resp.uid_byte = (stage <= MS1_REG_STAGE_UID0) ? m->config.uid[stage] : 0;
    resp.stage = stage;
    resp.direction = MS1_DIR_FROM_MASTER;
    resp.node = MS1_NODE_REG;

    uint8_t data[8];
    memset(data, 0, 8);

    switch (stage) {
    case MS1_REG_STAGE_UID2: /* Stage 1 */
    case MS1_REG_STAGE_UID1: /* Stage 2 */
    case MS1_REG_STAGE_UID0: /* Stage 3 */
        /* Echo with master data */
        data[0] = m->config.uid[0];
        data[1] = m->config.uid[1];
        data[2] = m->config.uid[2];
        data[3] = m->config.uid[3];
        break;

    case MS1_REG_STAGE_HW: /* Stage 4 */
        /* NMRA Hersteller-ID, master type codes */
        data[0] = 0x01;  /* Master type indicator */
        data[1] = 0xC0;  /* CS1/ECoS master */
        data[2] = 0x01;
        data[3] = m->config.vendor_id;
        data[4] = 0x00;
        data[5] = 0x01; /* CS-Master */
        data[6] = 0x01; /* CS-Master */
        data[7] = 0x03; /* EC-Master */
        break;

    case MS1_REG_STAGE_SW: /* Stage 5 */
        /* HW version */
        data[4] = (uint8_t)(m->config.hw_version >> 8);
        data[5] = (uint8_t)(m->config.hw_version & 0xFF);
        data[7] = 0x10;
        break;

    case MS1_REG_STAGE_BL: /* Stage 6 */
        /* SW version + master type */
        data[2] = (uint8_t)(m->config.sw_version >> 8);
        data[3] = (uint8_t)(m->config.sw_version & 0xFF);
        data[7] = m->config.master_type;
        break;

    case MS1_REG_STAGE_NODE: { /* Stage 7 */
        /* Assign node number + start object handle */
        uint8_t base = m->config.node_base + (m->next_node_pair * 2);
        slave->master_node = base;
        slave->slave_node = base + 1;
        m->next_node_pair++;

        slave->start_obj_handle = 0x0001;
        slave->sd_handle = 0x0008;  /* CS1/ECoS style */
        slave->sy_handle = 0x0003;  /* CS1/ECoS style */

        data[0] = 0x00;
        data[1] = 0x00;
        data[2] = 0x00;
        data[3] = 0x00;
        /* Data[4-5]: Start-Obj-Handle */
        ms1_data_put_u16(data, 4, slave->start_obj_handle);
        data[6] = 0x00;
        /* Data[7]: Node number for slave (lower 7 bits) + multi flag (bit 7) */
        data[7] = slave->slave_node & 0x7F;

        slave->state = MS1_SLAVE_SETUP;
        slave->last_heartbeat_ms = m->platform->millis();

        master_send_frame(m, ms1_id_encode_reg(&resp), 8, data);
        return;
    }
    }

    master_send_frame(m, ms1_id_encode_reg(&resp), 8, data);

    /* Advance state */
    if (stage < MS1_REG_STAGE_NODE) {
        slave->state = (ms1_slave_state_t)(MS1_SLAVE_REG_STAGE_0 + stage + 1);
    }
}

/* ========================================================================
 * Operational frame dispatcher
 * ====================================================================== */

static void master_handle_operational(ms1_master_t *m, const ms1_can_frame_t *frame)
{
    ms1_op_id_t oid;
    ms1_id_decode_op(frame->id, &oid);

    /* Find slave by node number (frames come from slave node) */
    uint8_t slave_id;
    ms1_slave_ctx_t *slave = master_find_slave_by_node(m, oid.node, &slave_id);
    if (!slave) {
        return;
    }

    /* Update heartbeat timestamp for any frame from this slave */
    slave->last_heartbeat_ms = m->platform->millis();

    /* Dispatch by priority and command */
    switch (oid.priority) {
    case MS1_PRIO_DATA:
        /* Setup / data exchange frames */
        master_handle_setup_frame(m, slave, slave_id, &oid, frame);
        break;

    case MS1_PRIO_HEARTBEAT:
        /* Cyclic monitoring */
        master_handle_heartbeat_frame(m, slave, slave_id, &oid, frame);
        break;

    case MS1_PRIO_SLAVE_DRIVE:
    case MS1_PRIO_DIRECT_LOK:
        /* Lok control from slave */
        master_handle_control_frame(m, slave, slave_id, &oid, frame);
        break;

    case MS1_PRIO_ASSIGNMENT:
        /* Lok assignment */
        master_handle_assignment_frame(m, slave, slave_id, &oid, frame);
        break;

    case MS1_PRIO_STOP_GO:
        /* Stop / Go from slave */
        master_handle_stop_go_frame(m, slave, slave_id, &oid, frame);
        break;

    default:
        break;
    }
}

/* ========================================================================
 * Setup frame handler (priority = 0b110)
 *
 * The slave sends queries during setup, the master responds.
 * This handler is also active during operational phase for dynamic queries.
 * ====================================================================== */

static void master_handle_setup_frame(ms1_master_t *m, ms1_slave_ctx_t *slave,
                                      uint8_t slave_id, const ms1_op_id_t *oid,
                                      const ms1_can_frame_t *frame)
{
    uint16_t handle = oid->handle;
    uint8_t cmd = oid->command;
    const uint8_t *d = frame->data;

    /* --- SD-Handle request (cmd=OBJECT2, data[0]=0x40) --- */
    if (cmd == MS1_CMD_OBJECT2 && frame->dlc >= 2 && d[0] == MS1_DATA_SD_REQUEST) {
        /* Slave requests the SD (Slave Description) handle */
        uint8_t resp_data[8];
        memset(resp_data, 0, 8);
        resp_data[0] = MS1_DATA_SD_REQUEST; /* 0x40 */
        resp_data[1] = d[1]; /* Knot echo */
        resp_data[2] = 0x00;
        resp_data[3] = 0x00;
        ms1_data_put_u16(resp_data, 4, slave->sd_handle);
        resp_data[6] = 0x00;
        resp_data[7] = 0x01;

        master_send_op_frame(m, MS1_PRIO_DATA, handle,
                             MS1_CMD_OBJECT2, slave->master_node, 8, resp_data);
        slave->setup_sd_done = true;
        return;
    }

    /* --- Lokstack size query (cmd=STRING, handle=SD, data[0]=0x41) --- */
    if (cmd == MS1_CMD_STRING && handle == slave->sd_handle &&
        frame->dlc >= 1 && d[0] == MS1_DATA_LOKSTACK_SZ) {
        uint8_t resp_data[8];
        memset(resp_data, 0, 8);
        resp_data[0] = MS1_DATA_LOKSTACK_SZ;
        resp_data[5] = MS1_LOKSTACK_SIZE; /* lokstack size = 10 */

        master_send_op_frame(m, MS1_PRIO_DATA, slave->sd_handle,
                             MS1_CMD_STRING, slave->master_node, 6, resp_data);
        return;
    }

    /* --- Slave name query/set (cmd=STRING, handle=SD, data[0]=0x02) --- */
    if (cmd == MS1_CMD_STRING && handle == slave->sd_handle &&
        frame->dlc >= 2 && d[0] == MS1_DATA_NAME) {
        uint8_t offset = d[1];
        if (frame->dlc <= 4) {
            uint8_t resp_data[8];
            memset(resp_data, 0, 8);
            resp_data[0] = MS1_DATA_NAME;
            resp_data[1] = offset;
            master_send_op_frame(m, MS1_PRIO_DATA, slave->sd_handle,
                                 MS1_CMD_STRING, slave->master_node, 8, resp_data);
        }
        return;
    }

    /* --- System-Status-Handle request (cmd=OBJECT, data[0]=0x03) --- */
    if (cmd == MS1_CMD_OBJECT && frame->dlc >= 2 && d[0] == MS1_DATA_SY_REQUEST) {
        uint8_t resp_data[8];
        memset(resp_data, 0, 8);
        resp_data[0] = MS1_DATA_SD_REQUEST; /* 0x40 */
        resp_data[1] = MS1_DATA_SY_REQUEST; /* 0x03 */
        resp_data[2] = 0x00;
        resp_data[3] = 0x00;
        ms1_data_put_u16(resp_data, 4, slave->sy_handle);
        resp_data[6] = 0x00;
        resp_data[7] = 0x01;

        master_send_op_frame(m, MS1_PRIO_DATA, handle,
                             MS1_CMD_OBJECT, slave->master_node, 8, resp_data);
        slave->setup_sy_done = true;
        return;
    }

    /* --- System status queries (cmd=LIST, handle=SY) --- */
    if (cmd == MS1_CMD_LIST && handle == slave->sy_handle && frame->dlc >= 2) {
        uint8_t idx = d[0];   /* Status index (1-based) */
        uint8_t qtype = d[1]; /* 0x00=value, 0x01=type */

        uint8_t resp_data[8];
        memset(resp_data, 0, 8);
        resp_data[0] = idx;
        resp_data[1] = qtype;

        if (qtype == 0x01) {
            /* Type query */
            switch (idx) {
            case 1:
                resp_data[2] = MS1_STATUS_STOP_GO;
                break;
            case 2:
                resp_data[2] = MS1_STATUS_OVERCURRENT;
                break;
            case 3:
                resp_data[2] = MS1_STATUS_MFX_REG;
                break;
            default:
                /* End of status list: respond with empty (no data[2]) */
                master_send_op_frame(m, MS1_PRIO_DATA, slave->sy_handle,
                                     MS1_CMD_LIST, slave->master_node, 2, resp_data);
                slave->setup_status_done = true;
                return;
            }
            master_send_op_frame(m, MS1_PRIO_DATA, slave->sy_handle,
                                 MS1_CMD_LIST, slave->master_node, 3, resp_data);
        } else {
            /* Value query */
            switch (idx) {
            case 1:
                resp_data[2] = m->stopped ? 0x01 : 0x00;
                break;
            case 2:
                resp_data[2] = m->overcurrent ? 0x01 : 0x00;
                break;
            case 3:
                resp_data[2] = 0x00; /* mfx registration not active */
                break;
            default:
                master_send_op_frame(m, MS1_PRIO_DATA, slave->sy_handle,
                                     MS1_CMD_LIST, slave->master_node, 2, resp_data);
                return;
            }
            master_send_op_frame(m, MS1_PRIO_DATA, slave->sy_handle,
                                 MS1_CMD_LIST, slave->master_node, 3, resp_data);
        }
        return;
    }

    /* --- Lokstack exchange (cmd=OBJECT, handle=SY, data[0]=0x80) --- */
    if (cmd == MS1_CMD_OBJECT && handle == slave->sy_handle &&
        frame->dlc >= 4 && d[0] == MS1_DATA_LOKSTACK) {
        uint16_t req_idx = ms1_data_get_u16(d, 2);

        if (req_idx == 0x0000 && d[1] == 0x02) {
            /* Initial request - send first lokstack entry */
            if (slave->lokstack_count > 0) {
                master_send_lokstack_entry(m, slave, 0);
            } else {
                /* Empty lokstack - send end marker */
                uint8_t resp_data[8];
                memset(resp_data, 0, 8);
                resp_data[0] = MS1_DATA_LOKSTACK;
                resp_data[1] = 0x02;
                master_send_op_frame(m, MS1_PRIO_DATA, slave->sy_handle,
                                     MS1_CMD_OBJECT, slave->master_node, 4, resp_data);
            }
        } else {
            /* Subsequent request - slave echoes previous idx, we send next */
            uint8_t next = 0;
            for (uint8_t i = 0; i < slave->lokstack_count; i++) {
                if (slave->lok_handles[i] == req_idx) {
                    next = i + 1;
                    break;
                }
            }
            if (next < slave->lokstack_count) {
                master_send_lokstack_entry(m, slave, next);
            } else {
                /* End of lokstack */
                uint8_t resp_data[8];
                memset(resp_data, 0, 8);
                resp_data[0] = MS1_DATA_LOKSTACK;
                resp_data[1] = 0x02;
                ms1_data_put_u16(resp_data, 2, req_idx);
                master_send_op_frame(m, MS1_PRIO_DATA, slave->sy_handle,
                                     MS1_CMD_OBJECT, slave->master_node, 4, resp_data);
                slave->setup_lokstack_done = true;
            }
        }
        return;
    }

    /* --- Lok parameter queries (on LOK-Handle) --- */
    int8_t ls_idx = master_find_lokstack_by_handle(slave, handle);
    if (ls_idx < 0) {
        return;
    }
    int8_t lok_slot = slave->lokstack[ls_idx];
    if (lok_slot < 0 || lok_slot >= MS1_MAX_LOKS || !m->loks[lok_slot].active) {
        return;
    }

    /* --- Get Lok-Typ (cmd=STRING, data[0]=0x40, data[1]=0x03) --- */
    if (cmd == MS1_CMD_STRING && frame->dlc >= 2 &&
        d[0] == MS1_DATA_SD_REQUEST && d[1] == 0x03) {
        master_send_lok_type(m, slave, lok_slot, handle);
        return;
    }

    /* --- Request Schienenformat (cmd=STRING, data[0]=0x40, data[1]=0x01) --- */
    if (cmd == MS1_CMD_STRING && frame->dlc >= 2 &&
        d[0] == MS1_DATA_SD_REQUEST && d[1] == 0x01) {
        master_send_lok_format(m, slave, lok_slot, handle, d);
        return;
    }

    /* --- Function definition query (cmd=LIST, data[1]=0x01) --- */
    if (cmd == MS1_CMD_LIST && frame->dlc >= 2 && d[1] == 0x01) {
        master_send_lok_func_def(m, slave, lok_slot, handle, d[0]);
        return;
    }

    /* --- Function value query (cmd=LIST, data[1]=0x00) --- */
    if (cmd == MS1_CMD_LIST && frame->dlc >= 2 && d[1] == 0x00) {
        master_send_lok_func_value(m, slave, lok_slot, handle, d[0]);
        return;
    }

    /* --- Lok name query (cmd=STRING, data[0]=0x02) --- */
    if (cmd == MS1_CMD_STRING && frame->dlc >= 2 && d[0] == MS1_DATA_NAME) {
        master_send_lok_name_segment(m, slave, lok_slot, handle, d[1]);
        return;
    }

    /* --- Lok assignment request (cmd=STATUS, data[0]=0x03) --- */
    if (cmd == MS1_CMD_STATUS && frame->dlc >= 2 && d[0] == MS1_DATA_ASSIGNMENT) {
        master_send_lok_assignment(m, slave, lok_slot, handle);
        return;
    }
}

/* ========================================================================
 * Lokstack entry sender
 * ====================================================================== */

static void master_send_lokstack_entry(const ms1_master_t *m, ms1_slave_ctx_t *slave,
                                       uint8_t entry_idx)
{
    if (entry_idx >= slave->lokstack_count) return;

    uint8_t resp_data[8];
    memset(resp_data, 0, 8);
    resp_data[0] = MS1_DATA_LOKSTACK;
    resp_data[1] = 0x02;
    /* idx-H, idx-L: use the entry index as the identifier */
    ms1_data_put_u16(resp_data, 2, (uint16_t)(entry_idx + 1));
    /* lok-handle-H, lok-handle-L */
    ms1_data_put_u16(resp_data, 4, slave->lok_handles[entry_idx]);
    resp_data[6] = 0x00;
    resp_data[7] = 0x01;

    master_send_op_frame(m, MS1_PRIO_DATA, slave->sy_handle,
                         MS1_CMD_OBJECT, slave->master_node, 8, resp_data);
}

/* ========================================================================
 * Lok type response
 * ====================================================================== */

static void master_send_lok_type(const ms1_master_t *m, ms1_slave_ctx_t *slave,
                                 int8_t lok_slot, uint16_t lok_handle)
{
    const ms1_lok_t *lok = &m->loks[lok_slot];
    uint8_t resp_data[8];
    memset(resp_data, 0, 8);
    resp_data[0] = MS1_DATA_SD_REQUEST;
    resp_data[1] = 0x03;
    resp_data[4] = 0x00;
    resp_data[5] = (uint8_t)lok->type;

    /* Response uses SY-Handle per document observation */
    master_send_op_frame(m, MS1_PRIO_DATA, slave->sy_handle,
                         MS1_CMD_STRING, slave->master_node, 6, resp_data);
}

/* ========================================================================
 * Lok rail format response
 * ====================================================================== */

static void master_send_lok_format(const ms1_master_t *m, ms1_slave_ctx_t *slave,
                                   int8_t lok_slot, uint16_t lok_handle,
                                   const uint8_t *query_data)
{
    const ms1_lok_t *lok = &m->loks[lok_slot];
    uint8_t resp_data[8];
    memset(resp_data, 0, 8);
    resp_data[0] = MS1_DATA_SD_REQUEST;
    resp_data[1] = 0x01;
    resp_data[4] = query_data[2]; /* Echo query variant */
    resp_data[5] = (uint8_t)lok->format;
    resp_data[6] = 0x00;
    resp_data[7] = 0x00;

    master_send_op_frame(m, MS1_PRIO_DATA, lok_handle,
                         MS1_CMD_STRING, slave->master_node, 8, resp_data);
}

/* ========================================================================
 * Function definition response
 * ====================================================================== */

static void master_send_lok_func_def(const ms1_master_t *m, ms1_slave_ctx_t *slave,
                                     int8_t lok_slot, uint16_t lok_handle, uint8_t fkt)
{
    const ms1_lok_t *lok = &m->loks[lok_slot];

    /* Find this function in the lok's function list */
    for (uint8_t i = 0; i < lok->func_count; i++) {
        if (lok->funcs[i].number == fkt) {
            uint8_t resp_data[8];
            memset(resp_data, 0, 8);
            resp_data[0] = fkt;
            resp_data[1] = (uint8_t)lok->funcs[i].activation;
            resp_data[2] = (uint8_t)lok->funcs[i].icon;
            resp_data[3] = fkt;

            master_send_op_frame(m, MS1_PRIO_DATA, lok_handle,
                                 MS1_CMD_LIST, slave->master_node, 4, resp_data);
            return;
        }
    }

    /* Function not found - send end-of-list marker */
    uint8_t resp_data[8];
    memset(resp_data, 0, 8);
    resp_data[0] = fkt;
    resp_data[1] = fkt; /* Just echo the FKT */
    master_send_op_frame(m, MS1_PRIO_DATA, lok_handle,
                         MS1_CMD_LIST, slave->master_node, 2, resp_data);
}

/* ========================================================================
 * Function value response
 * ====================================================================== */

static void master_send_lok_func_value(const ms1_master_t *m, ms1_slave_ctx_t *slave,
                                       int8_t lok_slot, uint16_t lok_handle, uint8_t fkt)
{
    const ms1_lok_t *lok = &m->loks[lok_slot];

    for (uint8_t i = 0; i < lok->func_count; i++) {
        if (lok->funcs[i].number == fkt) {
            uint8_t resp_data[8];
            memset(resp_data, 0, 8);
            resp_data[0] = fkt;
            resp_data[1] = 0x00;
            resp_data[2] = lok->funcs[i].value;

            master_send_op_frame(m, MS1_PRIO_DATA, lok_handle,
                                 MS1_CMD_LIST, slave->master_node, 3, resp_data);
            return;
        }
    }
}

/* ========================================================================
 * Lok name segment response
 * ====================================================================== */

static void master_send_lok_name_segment(const ms1_master_t *m, ms1_slave_ctx_t *slave,
                                         int8_t lok_slot, uint16_t lok_handle,
                                         uint8_t offset)
{
    const ms1_lok_t *lok = &m->loks[lok_slot];
    uint8_t resp_data[8];
    memset(resp_data, 0, 8);
    resp_data[0] = MS1_DATA_NAME;
    resp_data[1] = offset;

    /* Copy up to 4 characters from the name at the given offset */
    size_t name_len = strlen(lok->name);
    for (uint8_t i = 0; i < MS1_NAME_SEGMENT_LEN; i++) {
        uint8_t pos = offset + i;
        if (pos < name_len) {
            resp_data[2 + i] = (uint8_t)lok->name[pos];
        } else {
            resp_data[2 + i] = 0x00;
        }
    }

    /* DLC varies: 6 for intermediate segments, full 8 is also OK */
    master_send_op_frame(m, MS1_PRIO_DATA, lok_handle,
                         MS1_CMD_STRING, slave->master_node, 8, resp_data);
}

/* ========================================================================
 * Lok assignment response
 * ====================================================================== */

static void master_send_lok_assignment(const ms1_master_t *m, ms1_slave_ctx_t *slave,
                                       int8_t lok_slot, uint16_t lok_handle)
{
    uint8_t resp_data[8];
    memset(resp_data, 0, 8);
    resp_data[0] = MS1_DATA_ASSIGNMENT;
    resp_data[1] = 0x00;

    /* Grant assignment: Data[2..3] = 0x00, Data[4] = 0x08 (permission) */
    resp_data[2] = 0x00;
    resp_data[3] = 0x08;
    resp_data[4] = 0x00;
    resp_data[5] = 0x01;

    master_send_op_frame(m, MS1_PRIO_DATA, lok_handle,
                         MS1_CMD_STATUS, slave->master_node, 6, resp_data);

    /* Mark this lok as selected by the slave */
    slave->selected_lok = lok_slot;

    /* If slave is in setup, check if we can transition to operational */
    if (slave->state == MS1_SLAVE_SETUP &&
        slave->setup_sd_done && slave->setup_sy_done) {
        slave->state = MS1_SLAVE_OPERATIONAL;
        if (m->callbacks.on_slave_connected) {
            for (uint8_t i = 0; i < MS1_MAX_SLAVES; i++) {
                if (&m->slaves[i] == slave) {
                    m->callbacks.on_slave_connected(m->callbacks.ctx, i);
                    break;
                }
            }
        }
    }
}

/* ========================================================================
 * Heartbeat handler (priority = 0b011, cmd = 0b111)
 * ====================================================================== */

static void master_handle_heartbeat_frame(ms1_master_t *m, ms1_slave_ctx_t *slave,
                                          uint8_t slave_id, const ms1_op_id_t *oid,
                                          const ms1_can_frame_t *frame)
{
    if (oid->command != MS1_CMD_MONITOR) {
        return;
    }

    slave->last_heartbeat_ms = m->platform->millis();
    slave->disconnect_retries = 0;

    /* Respond with master heartbeat */
    uint8_t resp_data[8];
    memset(resp_data, 0, 8);
    resp_data[0] = 0x03;
    resp_data[1] = 0x00;
    resp_data[2] = 0x00;
    resp_data[3] = frame->data[3]; /* Echo Knot */
    /* Data[4..7] = slave UID */
    resp_data[4] = slave->uid[0];
    resp_data[5] = slave->uid[1];
    resp_data[6] = slave->uid[2];
    resp_data[7] = slave->uid[3];

    master_send_op_frame(m, MS1_PRIO_HEARTBEAT, oid->handle,
                         MS1_CMD_MONITOR, slave->master_node, 8, resp_data);

    /* If slave was in setup and all setup done, transition to operational */
    if (slave->state == MS1_SLAVE_SETUP &&
        slave->setup_sd_done && slave->setup_sy_done &&
        slave->setup_status_done && slave->setup_lokstack_done) {
        slave->state = MS1_SLAVE_OPERATIONAL;
        if (m->callbacks.on_slave_connected) {
            m->callbacks.on_slave_connected(m->callbacks.ctx, slave_id);
        }
    }
}

/* ========================================================================
 * Control frame handler (priority = 0b010 or 0b001, cmd = LIST)
 * ====================================================================== */

static void master_handle_control_frame(ms1_master_t *m, ms1_slave_ctx_t *slave,
                                        uint8_t slave_id, const ms1_op_id_t *oid,
                                        const ms1_can_frame_t *frame)
{
    if (oid->command != MS1_CMD_LIST) {
        return;
    }
    if (frame->dlc < 3) {
        return;
    }

    uint16_t handle = oid->handle;
    uint8_t fkt = frame->data[0];
    uint8_t value = frame->data[2];

    /* Find which lok this handle belongs to */
    int8_t ls_idx = master_find_lokstack_by_handle(slave, handle);
    if (ls_idx < 0) return;
    int8_t lok_slot = slave->lokstack[ls_idx];
    if (lok_slot < 0 || lok_slot >= MS1_MAX_LOKS) return;

    if (fkt == 0x01) {
        /* Speed/direction control */
        /* Update stored value */
        for (uint8_t i = 0; i < m->loks[lok_slot].func_count; i++) {
            if (m->loks[lok_slot].funcs[i].number == 0x01) {
                m->loks[lok_slot].funcs[i].value = value;
                break;
            }
        }
        if (m->callbacks.on_lok_speed) {
            m->callbacks.on_lok_speed(m->callbacks.ctx, slave_id, (uint8_t)lok_slot, value);
        }
    } else {
        /* Function control */
        for (uint8_t i = 0; i < m->loks[lok_slot].func_count; i++) {
            if (m->loks[lok_slot].funcs[i].number == fkt) {
                m->loks[lok_slot].funcs[i].value = value;
                break;
            }
        }
        if (m->callbacks.on_lok_function) {
            m->callbacks.on_lok_function(m->callbacks.ctx, slave_id,
                                         (uint8_t)lok_slot, fkt, value);
        }
    }
}

/* ========================================================================
 * Assignment frame handler (priority = 0b101)
 * ====================================================================== */

static void master_handle_assignment_frame(ms1_master_t *m, ms1_slave_ctx_t *slave,
                                           uint8_t slave_id, const ms1_op_id_t *oid,
                                           const ms1_can_frame_t *frame)
{
    if (oid->command != MS1_CMD_STATUS) return;
    if (frame->dlc < 2) return;

    uint16_t handle = oid->handle;
    const uint8_t *d = frame->data;

    if (d[0] != MS1_DATA_ASSIGNMENT) return;

    int8_t ls_idx = master_find_lokstack_by_handle(slave, handle);
    if (ls_idx < 0) return;
    int8_t lok_slot = slave->lokstack[ls_idx];

    if (d[1] == 0x01) {
        /* Slave requests control of this lok */
        /* Respond with grant */
        uint8_t resp_data[8];
        memset(resp_data, 0, 8);
        resp_data[0] = MS1_DATA_ASSIGNMENT;
        resp_data[1] = 0x00;
        resp_data[2] = 0x00;
        resp_data[3] = 0x08;
        resp_data[4] = 0x00;
        resp_data[5] = 0x01;

        master_send_op_frame(m, MS1_PRIO_ASSIGNMENT, handle,
                             MS1_CMD_STATUS, slave->master_node, 6, resp_data);

        slave->selected_lok = lok_slot;
        if (m->callbacks.on_lok_selected) {
            m->callbacks.on_lok_selected(m->callbacks.ctx, slave_id, lok_slot);
        }
    } else if (d[1] == 0x00) {
        /* Slave releases this lok */
        uint8_t resp_data[8];
        memset(resp_data, 0, 8);
        resp_data[0] = MS1_DATA_ASSIGNMENT;
        resp_data[1] = 0x01;

        master_send_op_frame(m, MS1_PRIO_ASSIGNMENT, handle,
                             MS1_CMD_STATUS, slave->master_node, 6, resp_data);

        if (slave->selected_lok == lok_slot) {
            slave->selected_lok = -1;
        }
        if (m->callbacks.on_lok_selected) {
            m->callbacks.on_lok_selected(m->callbacks.ctx, slave_id, -1);
        }
    }
}

/* ========================================================================
 * Stop/Go frame handler (priority = 0b100 or 0b0010)
 * ====================================================================== */

static void master_handle_stop_go_frame(ms1_master_t *m, ms1_slave_ctx_t *slave,
                                        uint8_t slave_id, const ms1_op_id_t *oid,
                                        const ms1_can_frame_t *frame)
{
    if (oid->command != MS1_CMD_LIST) return;
    if (frame->dlc < 3) return;

    uint8_t idx = frame->data[0];
    uint8_t value = frame->data[2];

    if (idx == 0x01) {
        /* Stop/Go status change */
        if (value == 0x01) {
            m->stopped = true;
            if (m->callbacks.on_stop) {
                m->callbacks.on_stop(m->callbacks.ctx, slave_id);
            }
        } else {
            m->stopped = false;
            if (m->callbacks.on_go) {
                m->callbacks.on_go(m->callbacks.ctx, slave_id);
            }
        }
        /* Broadcast to all slaves */
        master_broadcast_status(m, 0x01, value);
    } else if (idx == 0x02) {
        /* Overcurrent status */
        m->overcurrent = (value != 0);
    }
}

/* ========================================================================
 * Flash handler
 * ====================================================================== */

static void master_handle_flash(ms1_master_t *m, const ms1_can_frame_t *frame)
{
    ms1_flash_id_t fid;
    ms1_id_decode_flash(frame->id, &fid);

    if (fid.priority != MS1_PRIO_REG_FLASH) return;

    /* Flash readiness signal from slave (handle=0, initial signal) */
    if (fid.flash_handle == 0x00 && !m->flash.active) {
        /* Slave is signaling flash readiness - extract UID from data */
        uint8_t slave_uid[4];
        slave_uid[0] = frame->data[0];
        slave_uid[1] = frame->data[1];
        slave_uid[2] = frame->data[2];
        slave_uid[3] = frame->data[3];

        /* Find which slave this is */
        uint8_t slave_id;
        ms1_slave_ctx_t *slave = master_find_slave_by_uid(m, slave_uid, &slave_id);

        /* Check if we have firmware and callback approves */
        bool allow = false;
        if (m->flash.fw_data && m->flash.fw_size > 0) {
            if (m->callbacks.on_flash_request) {
                allow = m->callbacks.on_flash_request(m->callbacks.ctx,
                                                       slave ? slave_id : 0xFF,
                                                       slave_uid);
            }
        }

        if (!allow) {
            /* Send flash abort */
            ms1_flash_id_t resp_fid;
            resp_fid.priority = MS1_PRIO_REG_FLASH;
            resp_fid.flash_handle = 0x00;
            resp_fid.request = 0;
            resp_fid.stage = 0;
            resp_fid.flags = 0;
            resp_fid.node = MS1_NODE_FLASH;

            uint8_t data[8];
            memcpy(data, slave_uid, 4);
            memset(data + 4, 0, 4);
            master_send_frame(m, ms1_id_encode_flash(&resp_fid), 4, data);
            return;
        }

        /* Assign flash handle */
        m->flash.active = true;
        m->flash.slave_id = slave ? slave_id : 0xFF;
        m->flash.flash_handle = m->next_flash_handle;
        m->next_flash_handle += 2;
        m->flash.query_stage = 0;
        m->flash.fw_offset = 0;

        /* Send flash handle assignment */
        ms1_flash_id_t resp_fid;
        resp_fid.priority = MS1_PRIO_REG_FLASH;
        resp_fid.flash_handle = m->flash.flash_handle;
        resp_fid.request = 0;
        resp_fid.stage = 0;
        resp_fid.flags = 0;
        resp_fid.node = MS1_NODE_FLASH;

        uint8_t data[8];
        memcpy(data, slave_uid, 4);
        memset(data + 4, 0, 4);
        master_send_frame(m, ms1_id_encode_flash(&resp_fid), 8, data);
        return;
    }

    /* Active flash session - handle subsequent frames */
    if (!m->flash.active || fid.flash_handle != m->flash.flash_handle) {
        return;
    }

    /* Vendor-ID / SW/HW-ID / Build queries from slave */
    if (m->flash.query_stage <= 2) {
        /* Slave sends ID info - we just acknowledge and advance */
        m->flash.query_stage++;

        if (m->flash.query_stage == 3) {
            /* All queries done - send flash authorization (Freigabe) */
            ms1_flash_id_t resp_fid;
            resp_fid.priority = MS1_PRIO_REG_FLASH;
            resp_fid.flash_handle = m->flash.flash_handle;
            resp_fid.request = 0;
            resp_fid.stage = 0;
            resp_fid.flags = 0;
            resp_fid.node = MS1_NODE_FLASH;

            uint8_t data[8];
            memset(data, 0, 8);
            /* Send slave UID */
            ms1_slave_ctx_t *slave = (m->flash.slave_id < MS1_MAX_SLAVES)
                                      ? &m->slaves[m->flash.slave_id] : NULL;
            if (slave) {
                memcpy(data, slave->uid, 4);
            }
            master_send_frame(m, ms1_id_encode_flash(&resp_fid), 4, data);
            m->flash.query_stage = 4; /* Ready for data transfer */
        }
        return;
    }

    /* Data request from slave (slave sends offset + count) */
    if (m->flash.query_stage == 4 && frame->dlc == 8) {
        uint32_t offset = ms1_data_get_u32(frame->data, 0);
        uint32_t count = ms1_data_get_u32(frame->data, 4);

        m->flash.block_offset = offset;
        m->flash.block_size = count;
        m->flash.fw_offset = offset;

        /* Send data in 4-byte chunks */
        uint32_t remaining = count;
        uint32_t pos = offset;

        while (remaining > 0) {
            uint8_t chunk_size = (remaining >= 4) ? 4 : (uint8_t)remaining;

            ms1_flash_id_t data_fid;
            data_fid.priority = MS1_PRIO_REG_FLASH;
            data_fid.flash_handle = m->flash.flash_handle;
            data_fid.request = 0;
            data_fid.stage = 0;
            data_fid.flags = 2; /* Bit 9 set = master data transmission */
            data_fid.node = MS1_NODE_FLASH;

            uint8_t data[8];
            memset(data, 0, 8);
            ms1_data_put_u32(data, 0, pos);
            for (uint8_t i = 0; i < chunk_size && (pos + i) < m->flash.fw_size; i++) {
                data[4 + i] = m->flash.fw_data[pos + i];
            }

            master_send_frame(m, ms1_id_encode_flash(&data_fid), 8, data);

            pos += 4;
            remaining = (remaining >= 4) ? (remaining - 4) : 0;
        }
        return;
    }

    /* Check if slave requests more data or we're done */
    if (m->flash.query_stage == 4 && frame->dlc == 4) {
        uint32_t offset = ms1_data_get_u32(frame->data, 0);
        if (offset >= m->flash.fw_size) {
            /* Flash complete - send end frame */
            ms1_flash_id_t end_fid;
            end_fid.priority = MS1_PRIO_REG_FLASH;
            end_fid.flash_handle = m->flash.flash_handle;
            end_fid.request = 0;
            end_fid.stage = 0;
            end_fid.flags = 0;
            end_fid.node = MS1_NODE_FLASH;

            ms1_slave_ctx_t *slave = (m->flash.slave_id < MS1_MAX_SLAVES)
                                      ? &m->slaves[m->flash.slave_id] : NULL;
            uint8_t data[8];
            memset(data, 0, 8);
            if (slave) {
                memcpy(data, slave->uid, 4);
            }
            master_send_frame(m, ms1_id_encode_flash(&end_fid), 4, data);

            /* Reset flash state */
            m->flash.active = false;

            /* Slave will reset and re-register */
            if (slave) {
                slave->state = MS1_SLAVE_IDLE;
            }
        }
    }
}

/* ========================================================================
 * Heartbeat timeout check
 * ====================================================================== */

static void master_check_heartbeat_timeouts(ms1_master_t *m)
{
    uint32_t now = m->platform->millis();

    for (uint8_t i = 0; i < MS1_MAX_SLAVES; i++) {
        ms1_slave_ctx_t *slave = &m->slaves[i];
        if (slave->state != MS1_SLAVE_OPERATIONAL &&
            slave->state != MS1_SLAVE_SETUP) {
            continue;
        }

        uint32_t elapsed = now - slave->last_heartbeat_ms;
        if (elapsed < MS1_HEARTBEAT_TIMEOUT_MS) {
            continue;
        }

        if (slave->state == MS1_SLAVE_OPERATIONAL) {
            slave->state = MS1_SLAVE_DISCONNECTING;
            slave->disconnect_retries = 0;
        }

        if (slave->state == MS1_SLAVE_DISCONNECTING) {
            if (slave->disconnect_retries >= MS1_DISCONNECT_MAX_RETRIES) {
                /* Give up - slave is gone */
                slave->state = MS1_SLAVE_IDLE;
                if (m->callbacks.on_slave_disconnected) {
                    m->callbacks.on_slave_disconnected(m->callbacks.ctx, i);
                }
                continue;
            }

            /* Send disconnect query (heartbeat probe) */
            uint8_t data[4];
            memset(data, 0, 4);
            data[3] = slave->slave_node; /* Knot = slave node */

            ms1_op_id_t probe_id;
            probe_id.priority = MS1_PRIO_HEARTBEAT;
            probe_id.handle = slave->start_obj_handle;
            probe_id.command = MS1_CMD_MONITOR;
            probe_id.node = slave->master_node;

            master_send_frame(m, ms1_id_encode_op(&probe_id), 4, data);
            slave->disconnect_retries++;
            slave->last_heartbeat_ms = now; /* Reset to wait another interval */
        }
    }
}

/* ========================================================================
 * Slave lookup helpers
 * ====================================================================== */

static ms1_slave_ctx_t *master_find_slave_by_node(ms1_master_t *m, uint8_t node,
                                                    uint8_t *slave_id_out)
{
    for (uint8_t i = 0; i < MS1_MAX_SLAVES; i++) {
        if (m->slaves[i].state != MS1_SLAVE_IDLE &&
            m->slaves[i].slave_node == node) {
            if (slave_id_out) *slave_id_out = i;
            return &m->slaves[i];
        }
    }
    return NULL;
}

static ms1_slave_ctx_t *master_find_slave_by_uid(ms1_master_t *m, const uint8_t uid[4],
                                                   uint8_t *slave_id_out)
{
    for (uint8_t i = 0; i < MS1_MAX_SLAVES; i++) {
        if (m->slaves[i].state != MS1_SLAVE_IDLE &&
            memcmp(m->slaves[i].uid, uid, 4) == 0) {
            if (slave_id_out) *slave_id_out = i;
            return &m->slaves[i];
        }
    }
    return NULL;
}

static ms1_slave_ctx_t *master_alloc_slave(ms1_master_t *m, uint8_t *slave_id_out)
{
    for (uint8_t i = 0; i < MS1_MAX_SLAVES; i++) {
        if (m->slaves[i].state == MS1_SLAVE_IDLE) {
            if (slave_id_out) *slave_id_out = i;
            return &m->slaves[i];
        }
    }
    return NULL;
}

static int8_t master_find_lokstack_by_handle(const ms1_slave_ctx_t *slave, uint16_t handle)
{
    for (uint8_t i = 0; i < slave->lokstack_count; i++) {
        if (slave->lok_handles[i] == handle) {
            return (int8_t)i;
        }
    }
    return -1;
}

/* ========================================================================
 * Status broadcast to all connected slaves
 * ====================================================================== */

static void master_broadcast_status(ms1_master_t *m, uint8_t status_idx, uint8_t value)
{
    for (uint8_t i = 0; i < MS1_MAX_SLAVES; i++) {
        ms1_slave_ctx_t *slave = &m->slaves[i];
        if (slave->state != MS1_SLAVE_OPERATIONAL) continue;

        uint8_t data[3];
        data[0] = status_idx;
        data[1] = 0x00;
        data[2] = value;

        master_send_op_frame(m, MS1_PRIO_STOP_GO, slave->sy_handle,
                             MS1_CMD_LIST, slave->master_node, 3, data);
    }
}

/* ========================================================================
 * Public API: Lok management
 * ====================================================================== */

bool ms1_master_set_lok(ms1_master_t *master, uint8_t slot, const ms1_lok_t *lok)
{
    if (slot >= MS1_MAX_LOKS) return false;

    if (lok) {
        master->loks[slot] = *lok;
        master->loks[slot].active = true;
    } else {
        memset(&master->loks[slot], 0, sizeof(ms1_lok_t));
        master->loks[slot].active = false;
    }
    return true;
}

bool ms1_master_remove_lok(ms1_master_t *master, uint8_t slot)
{
    return ms1_master_set_lok(master, slot, NULL);
}

bool ms1_master_assign_lok(ms1_master_t *master, uint8_t slave_id, uint8_t lok_slot)
{
    if (slave_id >= MS1_MAX_SLAVES) return false;
    if (lok_slot >= MS1_MAX_LOKS) return false;
    if (!master->loks[lok_slot].active) return false;

    ms1_slave_ctx_t *slave = &master->slaves[slave_id];
    if (slave->state != MS1_SLAVE_OPERATIONAL &&
        slave->state != MS1_SLAVE_SETUP) {
        return false;
    }

    /* Find a free lokstack slot */
    if (slave->lokstack_count >= MS1_LOKSTACK_SIZE) return false;

    /* Check if already assigned */
    for (uint8_t i = 0; i < slave->lokstack_count; i++) {
        if (slave->lokstack[i] == (int8_t)lok_slot) return false; /* Already there */
    }

    uint8_t idx = slave->lokstack_count;
    uint16_t handle = master->next_lok_handle++;
    slave->lokstack[idx] = (int8_t)lok_slot;
    slave->lok_handles[idx] = handle;
    slave->lokstack_count++;

    /* Send add-to-lokstack frame to slave */
    if (slave->state == MS1_SLAVE_OPERATIONAL) {
        uint8_t data[8];
        memset(data, 0, 8);
        data[0] = MS1_DATA_LOKSTACK;
        data[1] = 0x00;
        data[2] = 0xFF;
        data[3] = 0xFF;
        ms1_data_put_u16(data, 4, handle);
        data[6] = 0x00;
        data[7] = 0x03;

        master_send_op_frame(master, MS1_PRIO_DATA, slave->sy_handle,
                             MS1_CMD_OBJECT2, slave->master_node, 8, data);
    }

    return true;
}

bool ms1_master_unassign_lok(ms1_master_t *master, uint8_t slave_id, uint8_t lok_slot)
{
    if (slave_id >= MS1_MAX_SLAVES) return false;

    ms1_slave_ctx_t *slave = &master->slaves[slave_id];

    /* Find the lok in the lokstack */
    int8_t idx = -1;
    for (uint8_t i = 0; i < slave->lokstack_count; i++) {
        if (slave->lokstack[i] == (int8_t)lok_slot) {
            idx = (int8_t)i;
            break;
        }
    }
    if (idx < 0) return false;

    uint16_t handle = slave->lok_handles[idx];

    /* Send delete-from-lokstack frame */
    if (slave->state == MS1_SLAVE_OPERATIONAL) {
        uint8_t data[8];
        memset(data, 0, 8);
        data[0] = MS1_DATA_LOKSTACK;
        data[1] = 0x00;
        data[2] = 0x00;
        data[3] = 0x00;
        ms1_data_put_u16(data, 4, handle);
        data[6] = 0x00;
        data[7] = 0x01;

        master_send_op_frame(master, MS1_PRIO_DATA, slave->sy_handle,
                             MS1_CMD_DELETE, slave->master_node, 8, data);
    }

    /* Remove from lokstack (shift remaining entries) */
    for (uint8_t i = (uint8_t)idx; i < slave->lokstack_count - 1; i++) {
        slave->lokstack[i] = slave->lokstack[i + 1];
        slave->lok_handles[i] = slave->lok_handles[i + 1];
    }
    slave->lokstack_count--;
    slave->lokstack[slave->lokstack_count] = -1;

    if (slave->selected_lok == (int8_t)lok_slot) {
        slave->selected_lok = -1;
    }

    return true;
}

/* ========================================================================
 * Public API: System status
 * ====================================================================== */

void ms1_master_set_stop(ms1_master_t *master, bool stopped)
{
    master->stopped = stopped;
    master_broadcast_status(master, 0x01, stopped ? 0x01 : 0x00);
}

void ms1_master_set_overcurrent(ms1_master_t *master, bool fault)
{
    master->overcurrent = fault;
    master_broadcast_status(master, 0x02, fault ? 0x01 : 0x00);
}

/* ========================================================================
 * Public API: Master drives lok
 * ====================================================================== */

void ms1_master_drive_lok(ms1_master_t *master, uint8_t lok_slot, uint8_t speed_dir)
{
    if (lok_slot >= MS1_MAX_LOKS || !master->loks[lok_slot].active) return;

    /* Update stored value */
    for (uint8_t i = 0; i < master->loks[lok_slot].func_count; i++) {
        if (master->loks[lok_slot].funcs[i].number == 0x01) {
            master->loks[lok_slot].funcs[i].value = speed_dir;
            break;
        }
    }

    /* Send to all slaves that have this lok in their lokstack */
    for (uint8_t s = 0; s < MS1_MAX_SLAVES; s++) {
        ms1_slave_ctx_t *slave = &master->slaves[s];
        if (slave->state != MS1_SLAVE_OPERATIONAL) continue;

        for (uint8_t i = 0; i < slave->lokstack_count; i++) {
            if (slave->lokstack[i] == (int8_t)lok_slot) {
                uint8_t data[3];
                data[0] = 0x01; /* FKT = speed */
                data[1] = 0x00;
                data[2] = speed_dir;

                master_send_op_frame(master, MS1_PRIO_MASTER_DRIVE,
                                     slave->lok_handles[i], MS1_CMD_LIST,
                                     slave->master_node, 3, data);
                break;
            }
        }
    }
}

void ms1_master_drive_function(ms1_master_t *master, uint8_t lok_slot,
                                uint8_t func_nr, uint8_t value)
{
    if (lok_slot >= MS1_MAX_LOKS || !master->loks[lok_slot].active) return;

    /* Update stored value */
    for (uint8_t i = 0; i < master->loks[lok_slot].func_count; i++) {
        if (master->loks[lok_slot].funcs[i].number == func_nr) {
            master->loks[lok_slot].funcs[i].value = value;
            break;
        }
    }

    /* Send to all slaves */
    for (uint8_t s = 0; s < MS1_MAX_SLAVES; s++) {
        ms1_slave_ctx_t *slave = &master->slaves[s];
        if (slave->state != MS1_SLAVE_OPERATIONAL) continue;

        for (uint8_t i = 0; i < slave->lokstack_count; i++) {
            if (slave->lokstack[i] == (int8_t)lok_slot) {
                uint8_t data[3];
                data[0] = func_nr;
                data[1] = 0x00;
                data[2] = value;

                master_send_op_frame(master, MS1_PRIO_MASTER_DRIVE,
                                     slave->lok_handles[i], MS1_CMD_LIST,
                                     slave->master_node, 3, data);
                break;
            }
        }
    }
}

/* ========================================================================
 * Public API: Rename slave
 * ====================================================================== */

void ms1_master_rename_slave(ms1_master_t *master, uint8_t slave_id, const char *name)
{
    if (slave_id >= MS1_MAX_SLAVES) return;
    ms1_slave_ctx_t *slave = &master->slaves[slave_id];
    if (slave->state != MS1_SLAVE_OPERATIONAL &&
        slave->state != MS1_SLAVE_SETUP) {
        return;
    }

    size_t name_len = strlen(name);

    /* Send name in 4-character segments (up to 4 segments for 16 chars) */
    for (uint8_t offset = 0; offset < MS1_SLAVE_NAME_LEN; offset += MS1_NAME_SEGMENT_LEN) {
        uint8_t data[8];
        memset(data, 0, 8);
        data[0] = MS1_DATA_NAME;
        data[1] = offset;
        for (uint8_t i = 0; i < MS1_NAME_SEGMENT_LEN; i++) {
            uint8_t pos = offset + i;
            data[2 + i] = (pos < name_len) ? (uint8_t)name[pos] : 0x00;
        }

        master_send_op_frame(master, MS1_PRIO_DATA, slave->sd_handle,
                             MS1_CMD_STRING, slave->master_node, 8, data);
    }
}

/* ========================================================================
 * Public API: Get slave info
 * ====================================================================== */

bool ms1_master_get_slave_info(const ms1_master_t *master, uint8_t slave_id,
                                ms1_slave_info_t *info)
{
    if (slave_id >= MS1_MAX_SLAVES) return false;

    const ms1_slave_ctx_t *slave = &master->slaves[slave_id];
    info->state = slave->state;
    memcpy(info->uid, slave->uid, 4);
    info->master_node = slave->master_node;
    info->slave_node = slave->slave_node;
    memcpy(info->lokstack, slave->lokstack, sizeof(info->lokstack));
    info->lokstack_count = slave->lokstack_count;
    info->selected_lok = slave->selected_lok;
    return true;
}

/* ========================================================================
 * Public API: Load firmware for flash operations
 * ====================================================================== */

void ms1_master_load_firmware(ms1_master_t *master, const uint8_t *data, uint32_t size)
{
    master->flash.fw_data = data;
    master->flash.fw_size = size;
}
