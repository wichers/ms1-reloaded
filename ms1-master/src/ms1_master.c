/**
 * @file ms1_master.c
 * @brief M-CAN Master implementation.
 *
 * Implements the master side of the M-CAN protocol for controlling MS1 slaves,
 * frame for frame as the MS1's own master firmware does it, with one connection
 * (node pair) per slave as an ECoS/CS1 keeps them. Received frames are
 * dispatched by command field and handle; their priority is not evaluated.
 */

#include "ms1_master.h"
#include <string.h>

/* Lok data the library does not model, answered with the values the MS1
 * gives a locomotive it creates: maximum speed, acceleration, deceleration
 * (speed-object parameters 0-2) and volume. */
static const uint8_t LOK_SPEED_PARAMS[3] = {0xFF, 0x04, 0x03};
#define LOK_VOLUME          0xFF

/* Status objects 1..STATUS_OBJECTS; object i has kind i (ms1_status_type_t) */
#define STATUS_OBJECTS      4

/* Kind of a lok object in the lokstack iteration; 0x81 and 0x82 lists are empty */
#define KIND_LOK            MS1_DATA_LOKSTACK
#define KIND_LAST           0x82

/* ========================================================================
 * Frame helpers
 * ====================================================================== */

static uint32_t master_now(const ms1_master_t *m)
{
    return m->platform->millis();
}

static void master_send_frame(const ms1_master_t *m, uint32_t id, uint8_t dlc,
                              const uint8_t *data)
{
    ms1_can_frame_t frame;
    frame.id = id;
    frame.dlc = dlc;
    memset(frame.data, 0, 8);
    if (data && dlc > 0) {
        memcpy(frame.data, data, dlc);
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

/* A frame on this slave's connection: sent from its master node */
static void master_send_to(const ms1_master_t *m, const ms1_slave_ctx_t *slave,
                           uint8_t priority, uint16_t handle, uint8_t command,
                           uint8_t dlc, const uint8_t *data)
{
    master_send_op_frame(m, priority, handle, command, slave->master_node, dlc, data);
}

static size_t master_strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

static void master_log(const ms1_master_t *m, const char *msg)
{
    if (m->platform->log) {
        m->platform->log(msg);
    }
}

/* ========================================================================
 * Slaves and loks
 * ====================================================================== */

static bool master_slave_linked(const ms1_slave_ctx_t *slave)
{
    return slave->state == MS1_SLAVE_SETUP || slave->state == MS1_SLAVE_OPERATIONAL;
}

static bool master_slave_registering(const ms1_slave_ctx_t *slave)
{
    return slave->state >= MS1_SLAVE_REG_STAGE_0 && slave->state <= MS1_SLAVE_REG_STAGE_7;
}

static ms1_slave_ctx_t *master_find_slave_by_node(ms1_master_t *m, uint8_t node,
                                                    uint8_t *slave_id_out)
{
    for (uint8_t i = 0; i < MS1_MAX_SLAVES; i++) {
        if (master_slave_linked(&m->slaves[i]) && m->slaves[i].slave_node == node) {
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

static uint8_t master_slave_id(const ms1_master_t *m, const ms1_slave_ctx_t *slave)
{
    return (uint8_t)(slave - m->slaves);
}

static uint16_t master_lok_handle(uint8_t lok_slot)
{
    return (uint16_t)(MS1_HANDLE_LOK + lok_slot);
}

/* Index of a lok in the slave's lokstack, -1 if it is not there */
static int8_t master_lokstack_index(const ms1_slave_ctx_t *slave, uint8_t lok_slot)
{
    for (uint8_t i = 0; i < slave->lokstack_count; i++) {
        if (slave->lokstack[i] == (int8_t)lok_slot) {
            return (int8_t)i;
        }
    }
    return -1;
}

/* The lok slot a handle addresses, if the lok is in the slave's lokstack; -1 otherwise */
static int8_t master_lok_of_handle(const ms1_master_t *m, const ms1_slave_ctx_t *slave,
                                   uint16_t handle)
{
    if (handle < MS1_HANDLE_LOK || handle >= MS1_HANDLE_LOK + MS1_MAX_LOKS) {
        return -1;
    }
    uint8_t lok_slot = (uint8_t)(handle - MS1_HANDLE_LOK);
    if (!m->loks[lok_slot].active || master_lokstack_index(slave, lok_slot) < 0) {
        return -1;
    }
    return (int8_t)lok_slot;
}

static const ms1_lok_func_t *master_lok_func(const ms1_lok_t *lok, uint8_t number)
{
    for (uint8_t i = 0; i < lok->func_count && i < MS1_MAX_LOK_FUNCS; i++) {
        if (lok->funcs[i].number == number) {
            return &lok->funcs[i];
        }
    }
    return NULL;
}

/* Handle of the device controlling a lok: the controlling slave's own object, 0 if free */
static uint16_t master_lok_controller_handle(const ms1_master_t *m, uint8_t lok_slot)
{
    int8_t c = m->lok_controller[lok_slot];
    if (c < 0 || c >= MS1_MAX_SLAVES) {
        return 0;
    }
    return m->slaves[c].sd_handle;
}

/* ========================================================================
 * Frames about loks (to one slave)
 * ====================================================================== */

/* Announce a lok on the start object: {0x80, 0, controller, lok, counter} */
static void master_send_lok_announce(const ms1_master_t *m, const ms1_slave_ctx_t *slave,
                                     uint8_t lok_slot)
{
    uint8_t d[8];
    d[0] = KIND_LOK;
    d[1] = 0x00;
    ms1_data_put_u16(d, 2, master_lok_controller_handle(m, lok_slot));
    ms1_data_put_u16(d, 4, master_lok_handle(lok_slot));
    ms1_data_put_u16(d, 6, m->lok_change[lok_slot]);
    master_send_to(m, slave, MS1_PRIO_DATA, slave->start_obj_handle, MS1_CMD_OBJECT2, 8, d);
}

/* A lok leaves the lokstack: {0, 0, lok, counter} on the start object */
static void master_send_lok_remove(const ms1_master_t *m, const ms1_slave_ctx_t *slave,
                                   uint8_t lok_slot)
{
    uint8_t d[8] = {0};
    ms1_data_put_u16(d, 4, master_lok_handle(lok_slot));
    ms1_data_put_u16(d, 6, m->lok_change[lok_slot]);
    master_send_to(m, slave, MS1_PRIO_DATA, slave->start_obj_handle, MS1_CMD_DELETE, 8, d);
}

/* The lok's controller: {3, 0, controller, counter, 0} */
static void master_send_lok_controller(const ms1_master_t *m, const ms1_slave_ctx_t *slave,
                                       uint8_t lok_slot)
{
    uint8_t d[7];
    d[0] = MS1_DATA_ASSIGNMENT;
    d[1] = 0x00;
    ms1_data_put_u16(d, 2, master_lok_controller_handle(m, lok_slot));
    ms1_data_put_u16(d, 4, m->lok_change[lok_slot]);
    d[6] = 0x00; /* 1 would tell the slave that the master itself drives it */
    master_send_to(m, slave, MS1_PRIO_ASSIGNMENT, master_lok_handle(lok_slot),
                   MS1_CMD_STATUS, 7, d);
}

/* A speed or function value: {object, 0, value} */
static void master_send_lok_value(const ms1_master_t *m, const ms1_slave_ctx_t *slave,
                                  uint8_t lok_slot, uint8_t object, uint8_t value)
{
    uint8_t d[3] = {object, 0x00, value};
    master_send_to(m, slave, MS1_PRIO_STATE, master_lok_handle(lok_slot), MS1_CMD_LIST, 3, d);
}

/* Send a frame about a lok to every linked slave that has it */
static void master_lok_controller_to_all(const ms1_master_t *m, uint8_t lok_slot)
{
    for (uint8_t i = 0; i < MS1_MAX_SLAVES; i++) {
        const ms1_slave_ctx_t *s = &m->slaves[i];
        if (master_slave_linked(s) && master_lokstack_index(s, lok_slot) >= 0) {
            master_send_lok_controller(m, s, lok_slot);
        }
    }
}

static void master_lok_value_to_all(const ms1_master_t *m, uint8_t lok_slot,
                                    uint8_t object, uint8_t value)
{
    for (uint8_t i = 0; i < MS1_MAX_SLAVES; i++) {
        const ms1_slave_ctx_t *s = &m->slaves[i];
        if (master_slave_linked(s) && master_lokstack_index(s, lok_slot) >= 0) {
            master_send_lok_value(m, s, lok_slot, object, value);
        }
    }
}

/* The slave no longer controls its lok: tell the slaves that have it */
static void master_release_lok(ms1_master_t *m, ms1_slave_ctx_t *slave, bool notify)
{
    int8_t lok_slot = slave->selected_lok;
    slave->selected_lok = -1;
    if (lok_slot < 0 || m->lok_controller[lok_slot] != (int8_t)master_slave_id(m, slave)) {
        return;
    }
    m->lok_controller[lok_slot] = -1;
    if (notify) {
        master_lok_controller_to_all(m, (uint8_t)lok_slot);
    }
}

/* ========================================================================
 * Status
 * ====================================================================== */

static uint8_t master_status_value(const ms1_master_t *m, uint8_t object)
{
    switch (object) {
    case MS1_STATUS_STOP_GO:     return m->stopped ? 1 : 0;
    case MS1_STATUS_OVERCURRENT: return m->overcurrent ? 1 : 0;
    default:                     return 0;
    }
}

static void master_send_status(const ms1_master_t *m, const ms1_slave_ctx_t *slave,
                               uint8_t object)
{
    uint8_t d[3] = {object, 0x00, master_status_value(m, object)};
    master_send_to(m, slave, MS1_PRIO_STATE, slave->sy_handle, MS1_CMD_LIST, 3, d);
}

static void master_broadcast_status(const ms1_master_t *m, uint8_t object,
                                    const ms1_slave_ctx_t *except)
{
    for (uint8_t i = 0; i < MS1_MAX_SLAVES; i++) {
        const ms1_slave_ctx_t *s = &m->slaves[i];
        if (master_slave_linked(s) && s != except) {
            master_send_status(m, s, object);
        }
    }
}

/* ========================================================================
 * Initialization
 * ====================================================================== */

static void master_clear_slave(ms1_slave_ctx_t *slave)
{
    memset(slave, 0, sizeof(*slave));
    slave->state = MS1_SLAVE_IDLE;
    memset(slave->lokstack, -1, sizeof(slave->lokstack));
    slave->selected_lok = -1;
}

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

    for (int i = 0; i < MS1_MAX_SLAVES; i++) {
        master_clear_slave(&master->slaves[i]);
    }
    for (int i = 0; i < MS1_MAX_LOKS; i++) {
        master->loks[i].active = false;
        master->lok_change[i] = 0;
        master->lok_controller[i] = -1;
    }

    master->next_mid = 0;
    master->next_flash_handle = 0x02;
    master->bus_occupied_burst_remaining = MS1_BUS_OCCUPIED_BURST_COUNT;
}

/* ========================================================================
 * Slave gone
 * ====================================================================== */

static void master_drop_slave(ms1_master_t *m, ms1_slave_ctx_t *slave)
{
    bool was_connected = slave->state == MS1_SLAVE_OPERATIONAL;
    uint8_t slave_id = master_slave_id(m, slave);
    int8_t lok_slot = slave->selected_lok;

    master_release_lok(m, slave, false);
    master_clear_slave(slave);
    if (lok_slot >= 0) {
        master_lok_controller_to_all(m, (uint8_t)lok_slot); /* the other slaves: it is free */
    }
    if (was_connected && m->callbacks.on_slave_disconnected) {
        m->callbacks.on_slave_disconnected(m->callbacks.ctx, slave_id);
    }
}

/* ========================================================================
 * Registration handler
 *
 * Slave: stage 0-3 frames without data, its UID byte k in the handle; then
 * stages 4-7 with its UID, versions and configuration. Master: answers stages
 * 0-3 echoing UID byte and stage with its MID, and right after stage 3 sends
 * stage 7: the slave's UID, the start object and the node number.
 *
 * One registration at a time, as the MS1 master does: an MS1 slave in
 * registration restarts it on any registration answer that is not its own
 * (another stage, UID byte or MID, a stage 7 with another UID), so a stage-0
 * frame is ignored while a registration runs. Two slaves starting together
 * send the same stage-0 frame; the first stage-1 frame decides between them,
 * the other slave restarts and tries again after MS1_REG_TIMEOUT_MS.
 * ====================================================================== */

static void master_send_reg(const ms1_master_t *m, uint8_t uid_byte, uint8_t stage,
                            uint8_t mid, const uint8_t data[8])
{
    ms1_reg_id_t resp;
    resp.priority = MS1_PRIO_REG_FLASH;
    resp.uid_byte = uid_byte;
    resp.stage = stage;
    resp.mid = mid;
    resp.direction = MS1_DIR_FROM_MASTER;
    resp.node = MS1_NODE_REG;
    master_send_frame(m, ms1_id_encode_reg(&resp), 8, data);
}

/* Data of the answer to stage 0-3 */
static void master_reg_answer_data(const ms1_master_t *m, uint8_t stage, uint8_t d[8])
{
    const ms1_master_config_t *c = &m->config;
    memset(d, 0, 8);
    switch (stage) {
    case MS1_REG_STAGE_UID3:
        /* master UID; data[6..7] = handle of the cyclic monitoring */
        memcpy(d, c->uid, 4);
        d[5] = 0x02;
        ms1_data_put_u16(d, 6, MS1_HANDLE_MONITOR);
        break;
    case MS1_REG_STAGE_UID2:
        /* NMRA vendor, then the master family's bytes (protocol document) */
        d[3] = c->vendor_id;
        if (c->master_type == MS1_MASTER_TYPE_MS1) {
            d[4] = 0x01; d[5] = 0x00; d[6] = 0xC0; d[7] = 0x10;
        } else if (c->master_type == MS1_MASTER_TYPE_CS1) {
            d[6] = 0x01; d[7] = 0x01;
        } else {
            d[6] = 0x01; d[7] = 0x03;
        }
        break;
    case MS1_REG_STAGE_UID1:
        ms1_data_put_u16(d, 0, c->build_version);
        ms1_data_put_u16(d, 6, c->hw_version);
        break;
    case MS1_REG_STAGE_UID0:
        ms1_data_put_u16(d, 2, c->hw_version);
        ms1_data_put_u16(d, 4, c->sw_version);
        d[7] = c->master_type;
        break;
    default:
        break;
    }
}

/* Stage 7: node numbers, start object; data[7] = 0 (bit 0 would start the
 * slave's bootloader, bit 1 stop its registration) */
static void master_send_stage7(ms1_master_t *m, ms1_slave_ctx_t *slave, uint8_t slave_id)
{
    slave->master_node = (uint8_t)(m->config.node_base + slave_id * 2);
    slave->slave_node = (uint8_t)(slave->master_node + 1);
    slave->start_obj_handle = MS1_HANDLE_START;
    slave->sy_handle = MS1_HANDLE_STATUS;
    slave->sd_handle = (uint16_t)(MS1_HANDLE_DEVICE + slave->slave_node);
    slave->selected_lok = -1;
    slave->lokstack_count = 0;
    memset(slave->lokstack, -1, sizeof(slave->lokstack));

    uint8_t d[8];
    memcpy(d, slave->uid, 4);
    ms1_data_put_u16(d, 4, slave->start_obj_handle);
    /* bit 7: the master node is the slave node - 1 (else 0) */
    d[6] = (uint8_t)((slave->slave_node & 0x7F) | (slave->master_node != 0 ? 0x80 : 0x00));
    d[7] = 0x00;
    master_send_reg(m, 0x00, MS1_REG_STAGE_NODE, slave->mid, d);

    slave->state = MS1_SLAVE_SETUP;
    slave->last_heartbeat_ms = master_now(m);
    slave->last_poll_ms = slave->last_heartbeat_ms;
}

static void master_handle_registration(ms1_master_t *m, const ms1_can_frame_t *frame)
{
    ms1_reg_id_t rid;
    ms1_id_decode_reg(frame->id, &rid);

    /* Only slave frames: command field 0 (masters answer with 1) */
    if (rid.priority != MS1_PRIO_REG_FLASH || rid.direction != MS1_DIR_FROM_SLAVE) {
        return;
    }
    if (rid.stage > MS1_REG_STAGE_UID0) {
        return; /* stages 4-7: the slave's data; stage 7 is already sent */
    }

    ms1_slave_ctx_t *slave = NULL;
    uint8_t slave_id = 0;

    if (rid.stage == MS1_REG_STAGE_UID3) {
        /* First contact: a free context, unless a registration runs */
        for (uint8_t i = 0; i < MS1_MAX_SLAVES; i++) {
            if (master_slave_registering(&m->slaves[i])) {
                return;
            }
        }
        for (uint8_t i = 0; i < MS1_MAX_SLAVES; i++) {
            if (m->slaves[i].state == MS1_SLAVE_IDLE) {
                slave = &m->slaves[i];
                slave_id = i;
                break;
            }
        }
        if (!slave) {
            return; /* no free context; the slave tries again */
        }
        master_clear_slave(slave);
        slave->mid = m->next_mid;
        m->next_mid = (uint8_t)((m->next_mid + 1) & MS1_REG_MID_MASK);
        slave->state = MS1_SLAVE_REG_STAGE_0;
    } else {
        for (uint8_t i = 0; i < MS1_MAX_SLAVES; i++) {
            if ((int)m->slaves[i].state == (int)(MS1_SLAVE_REG_STAGE_0 + rid.stage) &&
                m->slaves[i].mid == rid.mid) {
                slave = &m->slaves[i];
                slave_id = i;
                break;
            }
        }
        if (!slave) {
            return;
        }
    }

    slave->uid[rid.stage] = rid.uid_byte;
    slave->last_poll_ms = master_now(m);

    uint8_t d[8];
    master_reg_answer_data(m, rid.stage, d);
    master_send_reg(m, rid.uid_byte, rid.stage, slave->mid, d);

    if (rid.stage < MS1_REG_STAGE_UID0) {
        slave->state = (ms1_slave_state_t)(MS1_SLAVE_REG_STAGE_0 + rid.stage + 1);
        return;
    }

    /* UID complete: a device registering again leaves its old connection */
    for (uint8_t i = 0; i < MS1_MAX_SLAVES; i++) {
        ms1_slave_ctx_t *old = &m->slaves[i];
        if (old != slave && old->state != MS1_SLAVE_IDLE &&
            memcmp(old->uid, slave->uid, 4) == 0) {
            master_drop_slave(m, old);
        }
    }
    master_send_stage7(m, slave, slave_id);
}

/* ========================================================================
 * Operational frames
 * ====================================================================== */

/* Type 7 on the monitor handle. The slave polls {0, master node} when the
 * master has been silent; the master answers {0, master node, master UID}.
 * {0, slave node, slave UID} is the slave's answer to the master's poll. */
static void master_handle_monitor(ms1_master_t *m, ms1_slave_ctx_t *slave,
                                  const ms1_op_id_t *oid, const ms1_can_frame_t *frame)
{
    const uint8_t *d = frame->data;
    if (oid->handle != MS1_HANDLE_MONITOR || frame->dlc < 4) {
        return;
    }
    if (ms1_data_get_u16(d, 0) != 0 || ms1_data_get_u16(d, 2) != slave->master_node) {
        return;
    }
    uint8_t r[8];
    ms1_data_put_u16(r, 0, 0);
    ms1_data_put_u16(r, 2, slave->master_node);
    memcpy(&r[4], m->config.uid, 4);
    master_send_to(m, slave, MS1_PRIO_HEARTBEAT, MS1_HANDLE_MONITOR, MS1_CMD_MONITOR, 8, r);
}

/* Type 4 on the start object: {0x40, slave node} asks for the slave's own
 * object; answer {0x40, node, 0, own object, its change counter} */
static void master_handle_announce(ms1_master_t *m, ms1_slave_ctx_t *slave,
                                   const ms1_op_id_t *oid, const ms1_can_frame_t *frame)
{
    const uint8_t *d = frame->data;
    if (oid->handle != slave->start_obj_handle || frame->dlc < 2 ||
        d[0] != MS1_DATA_SD_REQUEST) {
        return;
    }
    uint8_t r[8];
    r[0] = MS1_DATA_SD_REQUEST;
    r[1] = d[1];
    ms1_data_put_u16(r, 2, 0);
    ms1_data_put_u16(r, 4, slave->sd_handle);
    ms1_data_put_u16(r, 6, 0x0001);
    master_send_to(m, slave, MS1_PRIO_DATA, slave->start_obj_handle, MS1_CMD_OBJECT2, 8, r);
}

static void master_set_operational(ms1_master_t *m, ms1_slave_ctx_t *slave)
{
    if (slave->state != MS1_SLAVE_SETUP) {
        return;
    }
    slave->state = MS1_SLAVE_OPERATIONAL;
    if (m->callbacks.on_slave_connected) {
        m->callbacks.on_slave_connected(m->callbacks.ctx, master_slave_id(m, slave));
    }
}

/* Type 3 on the start object: the next object of a kind after a given one.
 * {3, 0}: the status object. {0x80..0x82, step, from}: the lokstack, one lok
 * per request; a 4-byte answer ends the list. */
static void master_handle_next_object(ms1_master_t *m, ms1_slave_ctx_t *slave,
                                      const ms1_op_id_t *oid, const ms1_can_frame_t *frame)
{
    const uint8_t *d = frame->data;
    if (oid->handle != slave->start_obj_handle || (frame->dlc != 2 && frame->dlc != 4)) {
        return;
    }
    uint16_t from = frame->dlc == 4 ? ms1_data_get_u16(d, 2) : 0;
    uint16_t next = 0;
    uint16_t change = 0;

    if (d[0] == MS1_DATA_SY_REQUEST) {
        if (from == 0) {
            next = slave->sy_handle;
            change = 0x0001;
        }
    } else if (d[0] == KIND_LOK) {
        int8_t i = -1;
        if (from == 0) {
            i = 0;
        } else {
            for (uint8_t k = 0; k < slave->lokstack_count; k++) {
                if (master_lok_handle((uint8_t)slave->lokstack[k]) == from) {
                    i = (int8_t)(k + 1);
                }
            }
        }
        if (i >= 0 && i < slave->lokstack_count) {
            next = master_lok_handle((uint8_t)slave->lokstack[i]);
            change = m->lok_change[slave->lokstack[i]];
        }
    } else if (d[0] < KIND_LOK || d[0] > KIND_LAST) {
        return;
    }

    uint8_t r[8];
    r[0] = d[0];
    r[1] = d[1];
    ms1_data_put_u16(r, 2, from);
    ms1_data_put_u16(r, 4, next);
    ms1_data_put_u16(r, 6, change);
    master_send_to(m, slave, MS1_PRIO_DATA, slave->start_obj_handle, MS1_CMD_OBJECT,
                   next ? 8 : 4, r);

    if (d[0] == KIND_LAST && next == 0) {
        master_set_operational(m, slave); /* the slave has its lokstack */
    }
}

/* Type 2 on the status object: {object, 1} kind, {object, 0} value,
 * {object, 0, value} the slave changes it (STOP / GO). The answer to a kind
 * query is followed by the value, as the MS1 master sends it. */
static void master_handle_status(ms1_master_t *m, ms1_slave_ctx_t *slave,
                                 uint8_t slave_id, const ms1_can_frame_t *frame)
{
    const uint8_t *d = frame->data;
    uint8_t object = d[0];
    if (frame->dlc < 2 || object == 0) {
        return;
    }
    if (d[1] == 0x01 && frame->dlc == 2) {
        uint8_t r[3] = {object, 0x01, object};
        if (object <= STATUS_OBJECTS) {
            master_send_to(m, slave, MS1_PRIO_DATA, slave->sy_handle, MS1_CMD_LIST, 3, r);
            master_send_status(m, slave, object);
        } else {
            master_send_to(m, slave, MS1_PRIO_DATA, slave->sy_handle, MS1_CMD_LIST, 2, r);
        }
        return;
    }
    if (d[1] != 0x00 || object > STATUS_OBJECTS) {
        return;
    }
    if (frame->dlc == 2) {
        master_send_status(m, slave, object);
        return;
    }

    uint8_t value = d[2];
    if (object == MS1_STATUS_STOP_GO) {
        /* bit 0 Stop, bit 1 emergency stop */
        m->stopped = (value & 0x03) != 0;
        if (m->stopped && m->callbacks.on_stop) {
            m->callbacks.on_stop(m->callbacks.ctx, slave_id);
        } else if (!m->stopped && m->callbacks.on_go) {
            m->callbacks.on_go(m->callbacks.ctx, slave_id);
        }
        master_broadcast_status(m, MS1_STATUS_STOP_GO, slave);
    } else if (object == MS1_STATUS_OVERCURRENT) {
        m->overcurrent = (value & 0x01) != 0;
        master_broadcast_status(m, MS1_STATUS_OVERCURRENT, slave);
    }
}

/* Type 2 on a lok: {object, 1} kind, {object, 0} value, {object, 0, value}
 * the slave drives, {speed object, 2, parameter, 0} speed-object parameter */
static void master_handle_lok_state(ms1_master_t *m, ms1_slave_ctx_t *slave,
                                    uint8_t slave_id, uint8_t lok_slot,
                                    const ms1_can_frame_t *frame)
{
    const uint8_t *d = frame->data;
    uint8_t object = d[0];
    ms1_lok_t *lok = &m->loks[lok_slot];
    uint16_t handle = master_lok_handle(lok_slot);
    if (frame->dlc < 2 || object == 0) {
        return;
    }
    const ms1_lok_func_t *f = master_lok_func(lok, object);

    if (d[1] == 0x01) {
        if (frame->dlc != 2) {
            return;
        }
        uint8_t r[4] = {object, 0x01, 0x00, 0x00};
        if (!f) {
            master_send_to(m, slave, MS1_PRIO_DATA, handle, MS1_CMD_LIST, 2, r); /* end */
            return;
        }
        if (f->activation == MS1_FUNC_SPEED) {
            r[2] = 0x01;
            r[3] = 0x01;
        } else {
            r[2] = 0x02;
            r[3] = (uint8_t)((f->icon & 0x7F) | (f->activation == MS1_FUNC_MOMENTARY ? 0x80 : 0x00));
        }
        master_send_to(m, slave, MS1_PRIO_DATA, handle, MS1_CMD_LIST, 4, r);
        return;
    }

    if (d[1] == 0x02) {
        if (frame->dlc != 4 || !f || f->activation != MS1_FUNC_SPEED || d[3] != 0 || d[2] > 2) {
            return;
        }
        uint8_t r[5] = {object, 0x02, d[2], 0x00, LOK_SPEED_PARAMS[d[2]]};
        master_send_to(m, slave, MS1_PRIO_DATA, handle, MS1_CMD_LIST, 5, r);
        return;
    }

    if (d[1] != 0x00 || !f) {
        return;
    }
    /* A value from a slave that does not control the lok is read as a query */
    if (frame->dlc < 3 || m->lok_controller[lok_slot] != (int8_t)slave_id) {
        master_send_lok_value(m, slave, lok_slot, object, f->value);
        return;
    }

    uint8_t value = d[2];
    for (uint8_t i = 0; i < lok->func_count && i < MS1_MAX_LOK_FUNCS; i++) {
        if (lok->funcs[i].number == object) {
            lok->funcs[i].value = (f->activation == MS1_FUNC_SPEED) ? value : (uint8_t)(value & 1);
        }
    }
    if (f->activation == MS1_FUNC_SPEED) {
        if (m->callbacks.on_lok_speed) {
            m->callbacks.on_lok_speed(m->callbacks.ctx, slave_id, lok_slot, value);
        }
    } else if (m->callbacks.on_lok_function) {
        m->callbacks.on_lok_function(m->callbacks.ctx, slave_id, lok_slot, object,
                                     (uint8_t)(value & 1));
    }
    /* The new value goes to every slave with the lok, the sender included */
    master_lok_value_to_all(m, lok_slot, object, f->value);
}

/* Type 1 on a lok: {group, item, sub, offset} reads lok data. Writes (more
 * data) are answered with the master's values. */
static void master_handle_lok_data(ms1_master_t *m, ms1_slave_ctx_t *slave,
                                   uint8_t lok_slot, const ms1_can_frame_t *frame)
{
    const uint8_t *d = frame->data;
    const ms1_lok_t *lok = &m->loks[lok_slot];
    uint16_t handle = master_lok_handle(lok_slot);
    uint8_t r[8] = {d[0], d[1], d[2], d[3], 0, 0, 0, 0};
    uint8_t dlc = 0;

    if (frame->dlc < 4) {
        return;
    }
    if (d[0] == MS1_DATA_SD_REQUEST && d[3] == 0) {
        if (d[1] == 0x03 && d[2] == 0x00) {
            r[5] = (uint8_t)lok->type;          /* symbol */
            dlc = 6;
        } else if (d[1] == 0x01 && d[2] == 0x00) {
            r[5] = (uint8_t)lok->format;        /* track protocol, flags 0 */
            dlc = 8;
        } else if (d[1] == 0x01 && d[2] == 0x01) {
            dlc = 8;                            /* track address 0, flags 0 (not modelled) */
        } else if (d[1] == 0x04 && d[2] == 0x00) {
            r[5] = LOK_VOLUME;
            dlc = 6;
        }
    } else if (d[0] == MS1_DATA_NAME && d[1] == 0x00 && d[2] == 0x00) {
        /* 4 characters from the offset, the terminating 0 included when it fits */
        size_t len = master_strnlen(lok->name, MS1_LOK_NAME_LEN);
        uint8_t off = d[3];
        uint8_t n = 0;
        while (n < 4 && (size_t)off + n <= len) {
            r[4 + n] = (off + n < len) ? (uint8_t)lok->name[off + n] : 0x00;
            n++;
        }
        dlc = (uint8_t)(4 + (n < 2 ? 2 : n));
    }
    if (dlc) {
        master_send_to(m, slave, MS1_PRIO_DATA, handle, MS1_CMD_STRING, dlc, r);
    }
}

/* Type 0 on a lok: {3, 0} who controls it; {3, 0, own object, counter} take;
 * {3, 1, own object, counter} release. The answer names the controller. */
static void master_handle_lok_controller(ms1_master_t *m, ms1_slave_ctx_t *slave,
                                         uint8_t slave_id, uint8_t lok_slot,
                                         const ms1_can_frame_t *frame)
{
    const uint8_t *d = frame->data;
    if (frame->dlc < 2 || d[0] != MS1_DATA_ASSIGNMENT || d[1] > 1) {
        return;
    }
    bool own = frame->dlc >= 4 && ms1_data_get_u16(d, 2) == slave->sd_handle;
    int8_t before = m->lok_controller[lok_slot];

    if (own && d[1] == 0x00) {
        if (before < 0) {
            if (slave->selected_lok >= 0 && slave->selected_lok != (int8_t)lok_slot) {
                master_release_lok(m, slave, true);
            }
            m->lok_controller[lok_slot] = (int8_t)slave_id;
            slave->selected_lok = (int8_t)lok_slot;
            if (m->callbacks.on_lok_selected) {
                m->callbacks.on_lok_selected(m->callbacks.ctx, slave_id, (int8_t)lok_slot);
            }
        }
    } else if (own && d[1] == 0x01) {
        if (before == (int8_t)slave_id) {
            master_release_lok(m, slave, false);
            if (m->callbacks.on_lok_selected) {
                m->callbacks.on_lok_selected(m->callbacks.ctx, slave_id, -1);
            }
        }
    }

    if (m->lok_controller[lok_slot] != before) {
        master_lok_controller_to_all(m, lok_slot);
    } else {
        master_send_lok_controller(m, slave, lok_slot);
    }
}

static void master_handle_operational(ms1_master_t *m, const ms1_can_frame_t *frame)
{
    ms1_op_id_t oid;
    ms1_id_decode_op(frame->id, &oid);

    uint8_t slave_id;
    ms1_slave_ctx_t *slave = master_find_slave_by_node(m, oid.node, &slave_id);
    if (!slave) {
        return;
    }
    slave->last_heartbeat_ms = master_now(m);

    switch (oid.command) {
    case MS1_CMD_MONITOR:
        master_handle_monitor(m, slave, &oid, frame);
        return;
    case MS1_CMD_OBJECT2:
        master_handle_announce(m, slave, &oid, frame);
        return;
    case MS1_CMD_OBJECT:
        master_handle_next_object(m, slave, &oid, frame);
        return;
    case MS1_CMD_LIST:
        if (oid.handle == slave->sy_handle) {
            master_handle_status(m, slave, slave_id, frame);
            return;
        }
        break;
    case MS1_CMD_STRING:
    case MS1_CMD_STATUS:
        break;
    default:
        return;
    }

    int8_t lok_slot = master_lok_of_handle(m, slave, oid.handle);
    if (lok_slot < 0) {
        return;
    }
    if (oid.command == MS1_CMD_LIST) {
        master_handle_lok_state(m, slave, slave_id, (uint8_t)lok_slot, frame);
    } else if (oid.command == MS1_CMD_STRING) {
        master_handle_lok_data(m, slave, (uint8_t)lok_slot, frame);
    } else {
        master_handle_lok_controller(m, slave, slave_id, (uint8_t)lok_slot, frame);
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
 * Timeouts and polls
 *
 * A registration that does not progress, or a slave that sends nothing, for
 * MS1_HEARTBEAT_TIMEOUT_MS is dropped (the slave restarts itself after the
 * same time without frames from the master). A slave silent for
 * MS1_HEARTBEAT_INTERVAL_MS is polled with {0, slave node, slave UID}, which
 * it answers with the same data.
 * ====================================================================== */

static void master_check_timeouts(ms1_master_t *m)
{
    uint32_t now = master_now(m);

    for (uint8_t i = 0; i < MS1_MAX_SLAVES; i++) {
        ms1_slave_ctx_t *slave = &m->slaves[i];

        if (master_slave_registering(slave)) {
            if (now - slave->last_poll_ms >= MS1_REG_TIMEOUT_MS) {
                master_log(m, "registration timed out");
                master_clear_slave(slave);
            }
            continue;
        }
        if (!master_slave_linked(slave)) {
            continue;
        }
        if (now - slave->last_heartbeat_ms >= MS1_HEARTBEAT_TIMEOUT_MS) {
            master_log(m, "slave timed out");
            master_drop_slave(m, slave);
            continue;
        }
        if (now - slave->last_heartbeat_ms >= MS1_HEARTBEAT_INTERVAL_MS &&
            now - slave->last_poll_ms >= MS1_HEARTBEAT_INTERVAL_MS) {
            uint8_t d[8];
            ms1_data_put_u16(d, 0, 0);
            ms1_data_put_u16(d, 2, slave->slave_node);
            memcpy(&d[4], slave->uid, 4);
            master_send_to(m, slave, MS1_PRIO_HEARTBEAT, MS1_HANDLE_MONITOR, MS1_CMD_MONITOR, 8, d);
            slave->last_poll_ms = now;
        }
    }
}

/* ========================================================================
 * Main poll loop
 * ====================================================================== */

void ms1_master_poll(ms1_master_t *master)
{
    /* Bus-occupied frames, once: 0x380 with zero data. To a slave whose
     * master node is 0 it is that master's reset request. */
    while (master->bus_occupied_burst_remaining > 0) {
        uint8_t data[8] = {0};
        master_send_frame(master, MS1_ID_BUS_OCCUPIED, 8, data);
        master->bus_occupied_burst_remaining--;
    }

    ms1_can_frame_t frame;
    while (master->platform->can_receive(&frame)) {
        uint8_t node = frame.id & 0x7Fu;
        if (node == MS1_NODE_REG) {
            master_handle_registration(master, &frame);
        } else if (node == MS1_NODE_FLASH) {
            master_handle_flash(master, &frame);
        } else {
            master_handle_operational(master, &frame);
        }
    }

    master_check_timeouts(master);
}

/* ========================================================================
 * Public API: Lok management
 * ====================================================================== */

bool ms1_master_set_lok(ms1_master_t *master, uint8_t slot, const ms1_lok_t *lok)
{
    if (slot >= MS1_MAX_LOKS) return false;

    if (!lok) {
        for (uint8_t i = 0; i < MS1_MAX_SLAVES; i++) {
            ms1_master_unassign_lok(master, i, slot);
        }
        memset(&master->loks[slot], 0, sizeof(ms1_lok_t));
        master->loks[slot].active = false;
        master->lok_controller[slot] = -1;
        return true;
    }

    bool update = master->loks[slot].active;
    master->loks[slot] = *lok;
    master->loks[slot].active = true;
    /* A new change counter makes the slaves fetch the lok again */
    if (++master->lok_change[slot] == 0) {
        master->lok_change[slot] = 1;
    }
    if (update) {
        for (uint8_t i = 0; i < MS1_MAX_SLAVES; i++) {
            ms1_slave_ctx_t *s = &master->slaves[i];
            if (master_slave_linked(s) && master_lokstack_index(s, slot) >= 0) {
                master_send_lok_announce(master, s, slot);
            }
        }
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
    if (!master_slave_linked(slave)) return false;
    if (slave->lokstack_count >= MS1_LOKSTACK_SIZE) return false;
    if (master_lokstack_index(slave, lok_slot) >= 0) return false;

    slave->lokstack[slave->lokstack_count++] = (int8_t)lok_slot;
    master_send_lok_announce(master, slave, lok_slot);
    return true;
}

bool ms1_master_unassign_lok(ms1_master_t *master, uint8_t slave_id, uint8_t lok_slot)
{
    if (slave_id >= MS1_MAX_SLAVES || lok_slot >= MS1_MAX_LOKS) return false;

    ms1_slave_ctx_t *slave = &master->slaves[slave_id];
    int8_t idx = master_lokstack_index(slave, lok_slot);
    if (idx < 0) return false;

    bool released = slave->selected_lok == (int8_t)lok_slot;
    if (released) {
        master_release_lok(master, slave, false);
    }
    if (master_slave_linked(slave)) {
        master_send_lok_remove(master, slave, lok_slot);
    }

    for (uint8_t i = (uint8_t)idx; i + 1 < slave->lokstack_count; i++) {
        slave->lokstack[i] = slave->lokstack[i + 1];
    }
    slave->lokstack_count--;
    slave->lokstack[slave->lokstack_count] = -1;

    /* Other slaves with the lok learn that it is free */
    if (released) {
        master_lok_controller_to_all(master, lok_slot);
    }
    return true;
}

/* ========================================================================
 * Public API: System status
 * ====================================================================== */

void ms1_master_set_stop(ms1_master_t *master, bool stopped)
{
    master->stopped = stopped;
    master_broadcast_status(master, MS1_STATUS_STOP_GO, NULL);
}

void ms1_master_set_overcurrent(ms1_master_t *master, bool fault)
{
    master->overcurrent = fault;
    master_broadcast_status(master, MS1_STATUS_OVERCURRENT, NULL);
}

/* ========================================================================
 * Public API: Master drives lok
 * ====================================================================== */

static void master_drive(ms1_master_t *master, uint8_t lok_slot, uint8_t number, uint8_t value)
{
    if (lok_slot >= MS1_MAX_LOKS || !master->loks[lok_slot].active) return;

    ms1_lok_t *lok = &master->loks[lok_slot];
    for (uint8_t i = 0; i < lok->func_count && i < MS1_MAX_LOK_FUNCS; i++) {
        if (lok->funcs[i].number == number) {
            lok->funcs[i].value = value;
        }
    }
    master_lok_value_to_all(master, lok_slot, number, value);
}

void ms1_master_drive_lok(ms1_master_t *master, uint8_t lok_slot, uint8_t speed_dir)
{
    if (lok_slot >= MS1_MAX_LOKS) return;
    const ms1_lok_t *lok = &master->loks[lok_slot];
    for (uint8_t i = 0; i < lok->func_count && i < MS1_MAX_LOK_FUNCS; i++) {
        if (lok->funcs[i].activation == MS1_FUNC_SPEED) {
            master_drive(master, lok_slot, lok->funcs[i].number, speed_dir);
            return;
        }
    }
}

void ms1_master_drive_function(ms1_master_t *master, uint8_t lok_slot,
                                uint8_t func_nr, uint8_t value)
{
    master_drive(master, lok_slot, func_nr, (uint8_t)(value & 1));
}

/* ========================================================================
 * Public API: Rename slave
 * ====================================================================== */

void ms1_master_rename_slave(ms1_master_t *master, uint8_t slave_id, const char *name)
{
    if (slave_id >= MS1_MAX_SLAVES) return;
    ms1_slave_ctx_t *slave = &master->slaves[slave_id];
    if (!master_slave_linked(slave)) return;

    /* {2, 0, 0, offset, 4 characters} to the slave's own object, the
     * terminating 0 in the last frame */
    size_t len = master_strnlen(name, MS1_SLAVE_NAME_LEN - 1);
    for (uint8_t off = 0; off <= len; off += MS1_NAME_SEGMENT_LEN) {
        uint8_t d[8] = {MS1_DATA_NAME, 0x00, 0x00, off, 0, 0, 0, 0};
        uint8_t n = 0;
        while (n < MS1_NAME_SEGMENT_LEN && (size_t)off + n <= len) {
            d[4 + n] = (off + n < len) ? (uint8_t)name[off + n] : 0x00;
            n++;
        }
        master_send_to(master, slave, MS1_PRIO_DATA, slave->sd_handle, MS1_CMD_STRING,
                       (uint8_t)(4 + n), d);
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
