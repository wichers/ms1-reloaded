/**
 * @file cs2_master.c
 * @brief CS2 CAN protocol master implementation.
 *
 * Implements the master (CS2-GUI central station) side of the CS2 CAN
 * protocol v2.0. Handles device discovery via ping, lok/accessory/system
 * commands, and config data streaming.
 */

#include "cs2_master.h"
#include <string.h>

/* ========================================================================
 * Internal helpers — forward declarations
 * ====================================================================== */

static void master_send_frame(const cs2_master_t *m, uint32_t id,
                               uint8_t dlc, const uint8_t *data);
static void master_send_command(const cs2_master_t *m, uint8_t priority,
                                 uint8_t command, uint8_t dlc,
                                 const uint8_t *data);
static void master_send_response(const cs2_master_t *m, uint8_t priority,
                                  uint8_t command, uint8_t dlc,
                                  const uint8_t *data);
static void master_send_response_hash(const cs2_master_t *m, uint8_t priority,
                                       uint8_t command, uint16_t hash,
                                       uint8_t dlc, const uint8_t *data);

static void master_handle_system(cs2_master_t *m, const cs2_can_id_t *id,
                                  const ms1_can_frame_t *frame);
static void master_handle_lok_speed(cs2_master_t *m, const cs2_can_id_t *id,
                                     const ms1_can_frame_t *frame);
static void master_handle_lok_direction(cs2_master_t *m, const cs2_can_id_t *id,
                                         const ms1_can_frame_t *frame);
static void master_handle_lok_function(cs2_master_t *m, const cs2_can_id_t *id,
                                        const ms1_can_frame_t *frame);
static void master_handle_acc_switch(cs2_master_t *m, const cs2_can_id_t *id,
                                      const ms1_can_frame_t *frame);
static void master_handle_s88_event(cs2_master_t *m, const cs2_can_id_t *id,
                                     const ms1_can_frame_t *frame);
static void master_handle_ping(cs2_master_t *m, const cs2_can_id_t *id,
                                const ms1_can_frame_t *frame);
static void master_handle_config_data_req(cs2_master_t *m, const cs2_can_id_t *id,
                                           const ms1_can_frame_t *frame);
static void master_handle_config_data_stream(cs2_master_t *m, const cs2_can_id_t *id,
                                              const ms1_can_frame_t *frame);
static void master_handle_status_config(cs2_master_t *m, const cs2_can_id_t *id,
                                         const ms1_can_frame_t *frame);
static void master_handle_update_offer(cs2_master_t *m, const cs2_can_id_t *id,
                                        const ms1_can_frame_t *frame);

static int master_find_device_by_uid(const cs2_master_t *m, uint32_t uid);
static int master_find_device_by_hash(const cs2_master_t *m, uint16_t hash);
static int master_alloc_device(cs2_master_t *m);
static int master_find_lok_by_locid(const cs2_master_t *m, uint32_t locid);
static void master_update_lok_state_speed(cs2_master_t *m, uint32_t locid,
                                           uint16_t speed);
static void master_update_lok_state_direction(cs2_master_t *m, uint32_t locid,
                                               cs2_direction_t dir);
static void master_update_lok_state_function(cs2_master_t *m, uint32_t locid,
                                              uint8_t func_nr, uint8_t value);

static void master_periodic_ping(cs2_master_t *m);
static void master_check_device_timeouts(cs2_master_t *m);
static void master_config_stream_tick(cs2_master_t *m);

/* ========================================================================
 * Initialization
 * ====================================================================== */

void cs2_master_init(cs2_master_t *m, const ms1_platform_t *platform,
                     const cs2_master_config_t *config,
                     const cs2_master_callbacks_t *callbacks)
{
    memset(m, 0, sizeof(*m));
    m->platform = platform;
    m->config = *config;
    if (callbacks) {
        m->callbacks = *callbacks;
    }
    m->hash = cs2_hash_from_uid(config->uid);
    m->state = CS2_STATE_IDLE;
    m->stopped = true;  /* Start with track power off */
    m->config_stream.state = CS2_CFG_IDLE;
}

/* ========================================================================
 * Poll loop
 * ====================================================================== */

void cs2_master_poll(cs2_master_t *m)
{
    ms1_can_frame_t frame;

    /* Process all incoming CAN frames */
    while (m->platform->can_receive(&frame)) {
        cs2_can_id_t id;
        cs2_id_decode(frame.id, &id);

        /* Check for hash collision — another device using our hash */
        if (id.hash == m->hash && id.response == 0) {
            /* A request with our hash from another device — possible collision.
             * We only flag it; real collision resolution would need UID comparison. */
        }

        /* Dispatch by command */
        switch (id.command) {
            case CS2_CMD_SYSTEM:
                master_handle_system(m, &id, &frame);
                break;
            case CS2_CMD_LOK_SPEED:
                master_handle_lok_speed(m, &id, &frame);
                break;
            case CS2_CMD_LOK_DIRECTION:
                master_handle_lok_direction(m, &id, &frame);
                break;
            case CS2_CMD_LOK_FUNCTION:
                master_handle_lok_function(m, &id, &frame);
                break;
            case CS2_CMD_ACC_SWITCH:
                master_handle_acc_switch(m, &id, &frame);
                break;
            case CS2_CMD_S88_EVENT:
                master_handle_s88_event(m, &id, &frame);
                break;
            case CS2_CMD_PING:
                master_handle_ping(m, &id, &frame);
                break;
            case CS2_CMD_CFG_DATA_REQ:
                master_handle_config_data_req(m, &id, &frame);
                break;
            case CS2_CMD_CFG_DATA_STREAM:
                master_handle_config_data_stream(m, &id, &frame);
                break;
            case CS2_CMD_STATUS_CONFIG:
                master_handle_status_config(m, &id, &frame);
                break;
            case CS2_CMD_UPDATE_OFFER:
                master_handle_update_offer(m, &id, &frame);
                break;
            default:
                break;
        }
    }

    /* Periodic tasks */
    master_periodic_ping(m);
    master_check_device_timeouts(m);
    master_config_stream_tick(m);
}

/* ========================================================================
 * Frame sending helpers
 * ====================================================================== */

static void master_send_frame(const cs2_master_t *m, uint32_t id,
                               uint8_t dlc, const uint8_t *data)
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

static void master_send_command(const cs2_master_t *m, uint8_t priority,
                                 uint8_t command, uint8_t dlc,
                                 const uint8_t *data)
{
    cs2_can_id_t id;
    id.priority = priority;
    id.command = command;
    id.response = 0;
    id.hash = m->hash;
    master_send_frame(m, cs2_id_encode(&id), dlc, data);
}

static void master_send_response(const cs2_master_t *m, uint8_t priority,
                                  uint8_t command, uint8_t dlc,
                                  const uint8_t *data)
{
    cs2_can_id_t id;
    id.priority = priority;
    id.command = command;
    id.response = 1;
    id.hash = m->hash;
    master_send_frame(m, cs2_id_encode(&id), dlc, data);
}

static void master_send_response_hash(const cs2_master_t *m, uint8_t priority,
                                       uint8_t command, uint16_t hash,
                                       uint8_t dlc, const uint8_t *data)
{
    cs2_can_id_t id;
    id.priority = priority;
    id.command = command;
    id.response = 1;
    id.hash = hash;
    master_send_frame(m, cs2_id_encode(&id), dlc, data);
}

/* ========================================================================
 * Lok database management
 * ====================================================================== */

bool cs2_master_set_lok(cs2_master_t *m, uint8_t slot, const cs2_lok_t *lok)
{
    if (slot >= CS2_MAX_LOKS) return false;
    m->loks[slot] = *lok;
    m->loks[slot].active = true;
    return true;
}

bool cs2_master_remove_lok(cs2_master_t *m, uint8_t slot)
{
    if (slot >= CS2_MAX_LOKS) return false;
    memset(&m->loks[slot], 0, sizeof(cs2_lok_t));
    return true;
}

/* ========================================================================
 * Lok control — outgoing commands
 * ====================================================================== */

void cs2_master_lok_speed(cs2_master_t *m, uint32_t locid, uint16_t speed)
{
    if (speed > CS2_SPEED_MAX) speed = CS2_SPEED_MAX;
    uint8_t data[6];
    ms1_data_put_u32(data, 0, locid);
    ms1_data_put_u16(data, 4, speed);
    master_send_command(m, CS2_PRIO_LOK_ACC, CS2_CMD_LOK_SPEED, 6, data);
    master_update_lok_state_speed(m, locid, speed);
}

void cs2_master_lok_direction(cs2_master_t *m, uint32_t locid, cs2_direction_t dir)
{
    uint8_t data[5];
    ms1_data_put_u32(data, 0, locid);
    data[4] = (uint8_t)dir;
    master_send_command(m, CS2_PRIO_LOK_ACC, CS2_CMD_LOK_DIRECTION, 5, data);
    master_update_lok_state_direction(m, locid, dir);
}

void cs2_master_lok_function(cs2_master_t *m, uint32_t locid,
                              uint8_t func_nr, uint8_t value)
{
    if (func_nr > CS2_FUNC_MAX) return;
    uint8_t data[6];
    ms1_data_put_u32(data, 0, locid);
    data[4] = func_nr;
    data[5] = value;
    master_send_command(m, CS2_PRIO_LOK_ACC, CS2_CMD_LOK_FUNCTION, 6, data);
    master_update_lok_state_function(m, locid, func_nr, value);
}

void cs2_master_lok_emergency_stop(cs2_master_t *m, uint32_t locid)
{
    uint8_t data[5];
    ms1_data_put_u32(data, 0, locid);
    data[4] = CS2_SYS_LOK_EMERGENCY;
    master_send_command(m, CS2_PRIO_STOP_GO, CS2_CMD_SYSTEM, 5, data);
}

/* ========================================================================
 * System commands
 * ====================================================================== */

void cs2_master_system_stop(cs2_master_t *m)
{
    uint8_t data[5];
    ms1_data_put_u32(data, 0, CS2_UID_BROADCAST);
    data[4] = CS2_SYS_STOP;
    master_send_command(m, CS2_PRIO_STOP_GO, CS2_CMD_SYSTEM, 5, data);
    m->stopped = true;
}

void cs2_master_system_go(cs2_master_t *m)
{
    uint8_t data[5];
    ms1_data_put_u32(data, 0, CS2_UID_BROADCAST);
    data[4] = CS2_SYS_GO;
    master_send_command(m, CS2_PRIO_STOP_GO, CS2_CMD_SYSTEM, 5, data);
    m->stopped = false;
    m->halt = false;
    if (m->state == CS2_STATE_IDLE || m->state == CS2_STATE_STOPPED) {
        m->state = CS2_STATE_RUNNING;
    }
}

void cs2_master_system_halt(cs2_master_t *m)
{
    uint8_t data[5];
    ms1_data_put_u32(data, 0, CS2_UID_BROADCAST);
    data[4] = CS2_SYS_HALT;
    master_send_command(m, CS2_PRIO_STOP_GO, CS2_CMD_SYSTEM, 5, data);
    m->halt = true;
}

/* ========================================================================
 * Accessory control
 * ====================================================================== */

void cs2_master_acc_switch(cs2_master_t *m, uint32_t locid,
                            uint8_t position, uint8_t power)
{
    uint8_t data[6];
    ms1_data_put_u32(data, 0, locid);
    data[4] = position;
    data[5] = power;
    master_send_command(m, CS2_PRIO_LOK_ACC, CS2_CMD_ACC_SWITCH, 6, data);
}

void cs2_master_acc_switch_timed(cs2_master_t *m, uint32_t locid,
                                  uint8_t position, uint8_t power,
                                  uint16_t time_10ms)
{
    uint8_t data[8];
    ms1_data_put_u32(data, 0, locid);
    data[4] = position;
    data[5] = power;
    ms1_data_put_u16(data, 6, time_10ms);
    master_send_command(m, CS2_PRIO_LOK_ACC, CS2_CMD_ACC_SWITCH, 8, data);
}

/* ========================================================================
 * S88 feedback
 * ====================================================================== */

void cs2_master_s88_subscribe(cs2_master_t *m, uint16_t device_id,
                               uint16_t contact)
{
    uint8_t data[4];
    ms1_data_put_u16(data, 0, device_id);
    ms1_data_put_u16(data, 2, contact);
    master_send_command(m, CS2_PRIO_FEEDBACK, CS2_CMD_S88_EVENT, 4, data);
}

void cs2_master_s88_query(cs2_master_t *m, uint16_t device_id,
                           uint16_t contact)
{
    uint8_t data[4];
    ms1_data_put_u16(data, 0, device_id);
    ms1_data_put_u16(data, 2, contact);
    master_send_command(m, CS2_PRIO_FEEDBACK, CS2_CMD_S88_EVENT, 4, data);
}

/* ========================================================================
 * Config data streaming
 * ====================================================================== */

void cs2_master_config_request(cs2_master_t *m, const char *filename)
{
    uint8_t data[8];
    memset(data, 0, 8);
    size_t len = strlen(filename);
    if (len > 8) len = 8;
    memcpy(data, filename, len);
    master_send_command(m, CS2_PRIO_LOK_ACC, CS2_CMD_CFG_DATA_REQ, 8, data);
}

bool cs2_master_config_provide(cs2_master_t *m, const char *filename,
                                const uint8_t *data, uint32_t length)
{
    if (m->config_stream.state != CS2_CFG_IDLE) return false;
    if (length > CS2_CONFIG_BUF_SIZE) return false;

    cs2_config_stream_t *s = &m->config_stream;
    s->state = CS2_CFG_SENDING;
    memset(s->filename, 0, sizeof(s->filename));
    size_t nlen = strlen(filename);
    if (nlen > 8) nlen = 8;
    memcpy(s->filename, filename, nlen);
    s->total_length = length;
    s->crc = cs2_crc16(data, length);
    memcpy(s->buffer, data, length);
    s->buffer_used = length;
    s->offset = 0;
    s->last_activity_ms = m->platform->millis();

    /* Send header packet: 4 bytes length + 2 bytes CRC */
    uint8_t hdr[6];
    ms1_data_put_u32(hdr, 0, length);
    ms1_data_put_u16(hdr, 4, s->crc);
    master_send_response_hash(m, CS2_PRIO_LOK_ACC, CS2_CMD_CFG_DATA_STREAM,
                               s->peer_hash, 6, hdr);
    return true;
}

/* ========================================================================
 * CV programming
 * ====================================================================== */

void cs2_master_read_config(cs2_master_t *m, uint32_t locid,
                             uint16_t cv_number, uint8_t count)
{
    uint8_t data[7];
    ms1_data_put_u32(data, 0, locid);
    ms1_data_put_u16(data, 4, cv_number);
    data[6] = count;
    master_send_command(m, CS2_PRIO_LOK_ACC, CS2_CMD_READ_CONFIG, 7, data);
}

void cs2_master_write_config(cs2_master_t *m, uint32_t locid,
                              uint16_t cv_number, uint8_t value)
{
    uint8_t data[8];
    ms1_data_put_u32(data, 0, locid);
    ms1_data_put_u16(data, 4, cv_number);
    data[6] = value;
    data[7] = 0;  /* control byte */
    master_send_command(m, CS2_PRIO_LOK_ACC, CS2_CMD_WRITE_CONFIG, 8, data);
}

/* ========================================================================
 * Discovery
 * ====================================================================== */

void cs2_master_ping(cs2_master_t *m)
{
    /* Send DLC=0 ping query — all devices respond with DLC=8 */
    master_send_command(m, CS2_PRIO_LOK_ACC, CS2_CMD_PING, 0, NULL);
    m->last_ping_ms = m->platform->millis();
    if (m->state == CS2_STATE_IDLE) {
        m->state = CS2_STATE_DISCOVERING;
    }
}

bool cs2_master_get_device(const cs2_master_t *m, uint8_t idx,
                            cs2_device_t *dev)
{
    if (idx >= CS2_MAX_DEVICES) return false;
    if (!m->devices[idx].present) return false;
    *dev = m->devices[idx];
    return true;
}

/* ========================================================================
 * Device lookup helpers
 * ====================================================================== */

static int master_find_device_by_uid(const cs2_master_t *m, uint32_t uid)
{
    for (int i = 0; i < CS2_MAX_DEVICES; i++) {
        if (m->devices[i].present && m->devices[i].uid == uid) {
            return i;
        }
    }
    return -1;
}

static int master_find_device_by_hash(const cs2_master_t *m, uint16_t hash)
{
    for (int i = 0; i < CS2_MAX_DEVICES; i++) {
        if (m->devices[i].present && m->devices[i].hash == hash) {
            return i;
        }
    }
    return -1;
}

static int master_alloc_device(cs2_master_t *m)
{
    for (int i = 0; i < CS2_MAX_DEVICES; i++) {
        if (!m->devices[i].present) {
            return i;
        }
    }
    return -1;
}

static int master_find_lok_by_locid(const cs2_master_t *m, uint32_t locid)
{
    for (int i = 0; i < CS2_MAX_LOKS; i++) {
        if (m->loks[i].active && m->loks[i].locid == locid) {
            return i;
        }
    }
    return -1;
}

/* ========================================================================
 * Lok state update helpers
 * ====================================================================== */

static void master_update_lok_state_speed(cs2_master_t *m, uint32_t locid,
                                           uint16_t speed)
{
    int idx = master_find_lok_by_locid(m, locid);
    if (idx >= 0) {
        m->loks[idx].speed = speed;
    }
}

static void master_update_lok_state_direction(cs2_master_t *m, uint32_t locid,
                                               cs2_direction_t dir)
{
    int idx = master_find_lok_by_locid(m, locid);
    if (idx >= 0) {
        if (dir == CS2_DIR_TOGGLE) {
            m->loks[idx].direction =
                (m->loks[idx].direction == CS2_DIR_FORWARD)
                    ? CS2_DIR_BACKWARD : CS2_DIR_FORWARD;
        } else if (dir != CS2_DIR_KEEP) {
            m->loks[idx].direction = dir;
        }
    }
}

static void master_update_lok_state_function(cs2_master_t *m, uint32_t locid,
                                              uint8_t func_nr, uint8_t value)
{
    int idx = master_find_lok_by_locid(m, locid);
    if (idx >= 0 && func_nr <= CS2_FUNC_MAX) {
        if (value) {
            m->loks[idx].functions |= (1u << func_nr);
        } else {
            m->loks[idx].functions &= ~(1u << func_nr);
        }
    }
}

/* ========================================================================
 * Command handlers — incoming frames
 * ====================================================================== */

static void master_handle_system(cs2_master_t *m, const cs2_can_id_t *id,
                                  const ms1_can_frame_t *frame)
{
    if (id->response) return;  /* We only process requests */
    if (frame->dlc < 5) return;

    uint32_t target_uid = ms1_data_get_u32(frame->data, 0);
    uint8_t sub_cmd = frame->data[4];

    /* Check if this targets us or is broadcast */
    if (target_uid != CS2_UID_BROADCAST && target_uid != m->config.uid) {
        return;
    }

    switch (sub_cmd) {
        case CS2_SYS_STOP:
            m->stopped = true;
            m->state = CS2_STATE_STOPPED;
            /* Send response */
            master_send_response(m, CS2_PRIO_STOP_GO, CS2_CMD_SYSTEM,
                                  frame->dlc, frame->data);
            if (m->callbacks.on_system_stop) {
                m->callbacks.on_system_stop(m->callbacks.ctx);
            }
            break;

        case CS2_SYS_GO:
            m->stopped = false;
            m->halt = false;
            m->state = CS2_STATE_RUNNING;
            master_send_response(m, CS2_PRIO_STOP_GO, CS2_CMD_SYSTEM,
                                  frame->dlc, frame->data);
            if (m->callbacks.on_system_go) {
                m->callbacks.on_system_go(m->callbacks.ctx);
            }
            break;

        case CS2_SYS_HALT:
            m->halt = true;
            master_send_response(m, CS2_PRIO_STOP_GO, CS2_CMD_SYSTEM,
                                  frame->dlc, frame->data);
            if (m->callbacks.on_system_halt) {
                m->callbacks.on_system_halt(m->callbacks.ctx);
            }
            break;

        case CS2_SYS_LOK_EMERGENCY: {
            uint32_t locid = ms1_data_get_u32(frame->data, 0);
            master_send_response(m, CS2_PRIO_STOP_GO, CS2_CMD_SYSTEM,
                                  frame->dlc, frame->data);
            master_update_lok_state_speed(m, locid, 0);
            if (m->callbacks.on_emergency_stop) {
                m->callbacks.on_emergency_stop(m->callbacks.ctx, locid);
            }
            break;
        }

        case CS2_SYS_OVERLOAD:
            if (frame->dlc >= 6) {
                uint8_t channel = frame->data[5];
                master_send_response(m, CS2_PRIO_STOP_GO, CS2_CMD_SYSTEM,
                                      frame->dlc, frame->data);
                if (m->callbacks.on_overload) {
                    m->callbacks.on_overload(m->callbacks.ctx, channel);
                }
            }
            break;

        default:
            /* Echo response for unknown sub-commands */
            master_send_response(m, CS2_PRIO_STOP_GO, CS2_CMD_SYSTEM,
                                  frame->dlc, frame->data);
            break;
    }
}

static void master_handle_lok_speed(cs2_master_t *m, const cs2_can_id_t *id,
                                     const ms1_can_frame_t *frame)
{
    if (frame->dlc < 4) return;

    uint32_t locid = ms1_data_get_u32(frame->data, 0);

    if (id->response) {
        /* Response: another device confirming a speed we set, or query result */
        if (frame->dlc >= 6) {
            uint16_t speed = ms1_data_get_u16(frame->data, 4);
            master_update_lok_state_speed(m, locid, speed);
        }
        return;
    }

    if (frame->dlc == 4) {
        /* DLC=4: Query — respond with current speed */
        int idx = master_find_lok_by_locid(m, locid);
        if (idx >= 0) {
            uint8_t resp[6];
            ms1_data_put_u32(resp, 0, locid);
            ms1_data_put_u16(resp, 4, m->loks[idx].speed);
            master_send_response(m, CS2_PRIO_LOK_ACC, CS2_CMD_LOK_SPEED,
                                  6, resp);
        }
    } else if (frame->dlc >= 6) {
        /* DLC=6: Set speed command */
        uint16_t speed = ms1_data_get_u16(frame->data, 4);
        if (speed > CS2_SPEED_MAX) speed = CS2_SPEED_MAX;

        master_update_lok_state_speed(m, locid, speed);

        /* Send response */
        uint8_t resp[6];
        ms1_data_put_u32(resp, 0, locid);
        ms1_data_put_u16(resp, 4, speed);
        master_send_response(m, CS2_PRIO_LOK_ACC, CS2_CMD_LOK_SPEED, 6, resp);

        if (m->callbacks.on_lok_speed) {
            m->callbacks.on_lok_speed(m->callbacks.ctx, locid, speed);
        }
    }
}

static void master_handle_lok_direction(cs2_master_t *m, const cs2_can_id_t *id,
                                         const ms1_can_frame_t *frame)
{
    if (frame->dlc < 4) return;

    uint32_t locid = ms1_data_get_u32(frame->data, 0);

    if (id->response) {
        if (frame->dlc >= 5) {
            cs2_direction_t dir = (cs2_direction_t)frame->data[4];
            master_update_lok_state_direction(m, locid, dir);
        }
        return;
    }

    if (frame->dlc == 4) {
        /* Query current direction */
        int idx = master_find_lok_by_locid(m, locid);
        if (idx >= 0) {
            uint8_t resp[5];
            ms1_data_put_u32(resp, 0, locid);
            resp[4] = (uint8_t)m->loks[idx].direction;
            master_send_response(m, CS2_PRIO_LOK_ACC, CS2_CMD_LOK_DIRECTION,
                                  5, resp);
        }
    } else if (frame->dlc >= 5) {
        /* Set direction */
        cs2_direction_t dir = (cs2_direction_t)frame->data[4];
        master_update_lok_state_direction(m, locid, dir);

        /* Respond with resolved direction */
        int idx = master_find_lok_by_locid(m, locid);
        uint8_t resp[5];
        ms1_data_put_u32(resp, 0, locid);
        resp[4] = (idx >= 0) ? (uint8_t)m->loks[idx].direction : (uint8_t)dir;
        master_send_response(m, CS2_PRIO_LOK_ACC, CS2_CMD_LOK_DIRECTION,
                              5, resp);

        if (m->callbacks.on_lok_direction) {
            cs2_direction_t resolved = (idx >= 0)
                ? m->loks[idx].direction : dir;
            m->callbacks.on_lok_direction(m->callbacks.ctx, locid, resolved);
        }
    }
}

static void master_handle_lok_function(cs2_master_t *m, const cs2_can_id_t *id,
                                        const ms1_can_frame_t *frame)
{
    if (frame->dlc < 4) return;

    uint32_t locid = ms1_data_get_u32(frame->data, 0);

    if (id->response) {
        if (frame->dlc >= 6) {
            uint8_t func_nr = frame->data[4];
            uint8_t value = frame->data[5];
            master_update_lok_state_function(m, locid, func_nr, value);
        }
        return;
    }

    if (frame->dlc == 5) {
        /* Query function value */
        uint8_t func_nr = frame->data[4];
        int idx = master_find_lok_by_locid(m, locid);
        if (idx >= 0 && func_nr <= CS2_FUNC_MAX) {
            uint8_t resp[6];
            ms1_data_put_u32(resp, 0, locid);
            resp[4] = func_nr;
            resp[5] = (m->loks[idx].functions & (1u << func_nr)) ? 1 : 0;
            master_send_response(m, CS2_PRIO_LOK_ACC, CS2_CMD_LOK_FUNCTION,
                                  6, resp);
        }
    } else if (frame->dlc >= 6) {
        /* Set function */
        uint8_t func_nr = frame->data[4];
        uint8_t value = frame->data[5];

        master_update_lok_state_function(m, locid, func_nr, value);

        uint8_t resp[6];
        ms1_data_put_u32(resp, 0, locid);
        resp[4] = func_nr;
        resp[5] = value;
        master_send_response(m, CS2_PRIO_LOK_ACC, CS2_CMD_LOK_FUNCTION,
                              6, resp);

        if (m->callbacks.on_lok_function) {
            m->callbacks.on_lok_function(m->callbacks.ctx, locid,
                                          func_nr, value);
        }
    }
}

static void master_handle_acc_switch(cs2_master_t *m, const cs2_can_id_t *id,
                                      const ms1_can_frame_t *frame)
{
    if (frame->dlc < 6) return;
    if (id->response) return;

    uint32_t locid = ms1_data_get_u32(frame->data, 0);
    uint8_t position = frame->data[4];
    uint8_t power = frame->data[5];

    /* Update accessory state */
    int found = -1;
    for (int i = 0; i < CS2_MAX_ACCESSORIES; i++) {
        if (m->accessories[i].active && m->accessories[i].locid == locid) {
            found = i;
            break;
        }
    }
    if (found >= 0) {
        m->accessories[found].position = position;
        m->accessories[found].power = power;
        if (frame->dlc >= 8) {
            m->accessories[found].switch_time = ms1_data_get_u16(frame->data, 6);
        }
    }

    /* Send response */
    master_send_response(m, CS2_PRIO_LOK_ACC, CS2_CMD_ACC_SWITCH,
                          frame->dlc, frame->data);

    if (m->callbacks.on_accessory_switch) {
        m->callbacks.on_accessory_switch(m->callbacks.ctx, locid,
                                          position, power);
    }
}

static void master_handle_s88_event(cs2_master_t *m, const cs2_can_id_t *id,
                                     const ms1_can_frame_t *frame)
{
    /* S88 events are typically responses with DLC=8:
     * data[0..1]=device_id, data[2..3]=contact, data[4]=old, data[5]=new,
     * data[6..7]=time */
    if (frame->dlc < 6) return;

    uint16_t device_id = ms1_data_get_u16(frame->data, 0);
    uint16_t contact = ms1_data_get_u16(frame->data, 2);
    uint8_t old_state = frame->data[4];
    uint8_t new_state = frame->data[5];

    if (m->callbacks.on_s88_event) {
        m->callbacks.on_s88_event(m->callbacks.ctx, device_id, contact,
                                   old_state, new_state);
    }
}

static void master_handle_ping(cs2_master_t *m, const cs2_can_id_t *id,
                                const ms1_can_frame_t *frame)
{
    if (id->response) {
        /* DLC=8 response: a device announcing itself */
        if (frame->dlc >= 8) {
            uint32_t uid = ms1_data_get_u32(frame->data, 0);
            uint16_t sw_ver = ms1_data_get_u16(frame->data, 4);
            uint16_t dev_type = ms1_data_get_u16(frame->data, 6);
            uint16_t dev_hash = id->hash;

            /* Check for hash collision with us */
            if (dev_hash == m->hash) {
                m->hash_collision = true;
            }

            /* Find or allocate device slot */
            int idx = master_find_device_by_uid(m, uid);
            if (idx < 0) {
                idx = master_alloc_device(m);
            }
            if (idx >= 0) {
                bool is_new = !m->devices[idx].present;
                m->devices[idx].present = true;
                m->devices[idx].uid = uid;
                m->devices[idx].sw_version = sw_ver;
                m->devices[idx].device_type = dev_type;
                m->devices[idx].hash = dev_hash;
                m->devices[idx].last_seen_ms = m->platform->millis();

                if (is_new && m->callbacks.on_device_found) {
                    m->callbacks.on_device_found(m->callbacks.ctx,
                                                  (uint8_t)idx,
                                                  &m->devices[idx]);
                }
            }
        }
        return;
    }

    /* DLC=0 request: someone is pinging. We respond with our own info. */
    if (frame->dlc == 0) {
        uint8_t resp[8];
        ms1_data_put_u32(resp, 0, m->config.uid);
        ms1_data_put_u16(resp, 4, m->config.sw_version);
        ms1_data_put_u16(resp, 6, m->config.device_type);
        master_send_response(m, CS2_PRIO_LOK_ACC, CS2_CMD_PING, 8, resp);
    }
}

static void master_handle_config_data_req(cs2_master_t *m, const cs2_can_id_t *id,
                                           const ms1_can_frame_t *frame)
{
    if (id->response) return;
    if (frame->dlc < 8) return;

    /* Extract 8-byte filename */
    char filename[9];
    memcpy(filename, frame->data, 8);
    filename[8] = '\0';
    /* Trim trailing zeros/spaces for clean string */
    for (int i = 7; i >= 0; i--) {
        if (filename[i] == '\0' || filename[i] == ' ') {
            filename[i] = '\0';
        } else {
            break;
        }
    }

    /* Remember the requester's hash for streaming back */
    m->config_stream.peer_hash = id->hash;

    if (m->callbacks.on_config_data_request) {
        m->callbacks.on_config_data_request(m->callbacks.ctx, filename);
    }
}

static void master_handle_config_data_stream(cs2_master_t *m, const cs2_can_id_t *id,
                                              const ms1_can_frame_t *frame)
{
    cs2_config_stream_t *s = &m->config_stream;

    if (s->state != CS2_CFG_RECEIVING && frame->dlc == 6 && !id->response) {
        /* Possibly a header from another device — not for us unless we requested */
        return;
    }

    if (s->state == CS2_CFG_RECEIVING) {
        s->last_activity_ms = m->platform->millis();

        if (frame->dlc == 6 && s->offset == 0) {
            /* Header packet: length(4) + CRC(2) */
            s->total_length = ms1_data_get_u32(frame->data, 0);
            s->crc = ms1_data_get_u16(frame->data, 4);
            s->offset = 0;
            s->buffer_used = 0;
            if (s->total_length > CS2_CONFIG_BUF_SIZE) {
                s->state = CS2_CFG_IDLE;  /* Too large */
            }
        } else if (frame->dlc == 8) {
            /* Data packet */
            uint32_t remaining = s->total_length - s->buffer_used;
            uint32_t copy = (remaining < 8) ? remaining : 8;
            if (s->buffer_used + copy <= CS2_CONFIG_BUF_SIZE) {
                memcpy(&s->buffer[s->buffer_used], frame->data, copy);
                s->buffer_used += copy;
            }

            /* Check if complete */
            if (s->buffer_used >= s->total_length) {
                /* Verify CRC */
                uint16_t calc_crc = cs2_crc16(s->buffer, s->total_length);
                if (calc_crc == s->crc && m->callbacks.on_config_data_received) {
                    m->callbacks.on_config_data_received(m->callbacks.ctx,
                                                          s->filename, s->buffer,
                                                          s->total_length);
                }
                s->state = CS2_CFG_IDLE;
            }
        }
    }
}

static void master_handle_status_config(cs2_master_t *m, const cs2_can_id_t *id,
                                         const ms1_can_frame_t *frame)
{
    /* Status configuration — acknowledge and pass through */
    if (id->response) return;
    master_send_response(m, CS2_PRIO_LOK_ACC, CS2_CMD_STATUS_CONFIG,
                          frame->dlc, frame->data);
}

static void master_handle_update_offer(cs2_master_t *m, const cs2_can_id_t *id,
                                        const ms1_can_frame_t *frame)
{
    if (id->response) return;
    if (frame->dlc < 4) return;

    uint32_t uid = ms1_data_get_u32(frame->data, 0);

    if (m->callbacks.on_update_offer) {
        m->callbacks.on_update_offer(m->callbacks.ctx, uid, frame->data);
    }
}

/* ========================================================================
 * Periodic tasks
 * ====================================================================== */

static void master_periodic_ping(cs2_master_t *m)
{
    uint32_t now = m->platform->millis();
    uint32_t elapsed = now - m->last_ping_ms;
    if (elapsed >= CS2_PING_INTERVAL_MS) {
        cs2_master_ping(m);
    }
}

static void master_check_device_timeouts(cs2_master_t *m)
{
    uint32_t now = m->platform->millis();
    for (int i = 0; i < CS2_MAX_DEVICES; i++) {
        if (m->devices[i].present) {
            uint32_t elapsed = now - m->devices[i].last_seen_ms;
            if (elapsed >= CS2_DEVICE_TIMEOUT_MS) {
                if (m->callbacks.on_device_lost) {
                    m->callbacks.on_device_lost(m->callbacks.ctx,
                                                 (uint8_t)i,
                                                 &m->devices[i]);
                }
                memset(&m->devices[i], 0, sizeof(cs2_device_t));
            }
        }
    }
}

static void master_config_stream_tick(cs2_master_t *m)
{
    cs2_config_stream_t *s = &m->config_stream;

    if (s->state == CS2_CFG_SENDING) {
        /* Check timeout */
        uint32_t now = m->platform->millis();
        if (now - s->last_activity_ms > CS2_CONFIG_STREAM_TIMEOUT_MS) {
            s->state = CS2_CFG_IDLE;
            return;
        }

        /* Send up to 4 data packets per tick (avoid bus congestion) */
        for (int i = 0; i < 4 && s->offset < s->total_length; i++) {
            uint8_t pkt[8];
            memset(pkt, 0, 8);
            uint32_t remaining = s->total_length - s->offset;
            uint32_t copy = (remaining < 8) ? remaining : 8;
            memcpy(pkt, &s->buffer[s->offset], copy);
            s->offset += copy;

            master_send_response_hash(m, CS2_PRIO_LOK_ACC,
                                       CS2_CMD_CFG_DATA_STREAM,
                                       s->peer_hash, 8, pkt);
            s->last_activity_ms = now;
        }

        /* Done? */
        if (s->offset >= s->total_length) {
            s->state = CS2_CFG_IDLE;
        }
    } else if (s->state == CS2_CFG_RECEIVING) {
        /* Check receive timeout */
        uint32_t now = m->platform->millis();
        if (now - s->last_activity_ms > CS2_CONFIG_STREAM_TIMEOUT_MS) {
            s->state = CS2_CFG_IDLE;
        }
    }
}
