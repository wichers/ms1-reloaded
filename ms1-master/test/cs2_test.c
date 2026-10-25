/**
 * @file cs2_test.c
 * @brief Tests for CS2 CAN protocol types and master implementation.
 *
 * Uses the same mock CAN bus infrastructure pattern as ms1_test_trace.c.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cs2_types.h"
#include "cs2_master.h"

/* ========================================================================
 * Test framework
 * ====================================================================== */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) static void name(void)
#define RUN_TEST(name) do { \
    printf("  %-50s ", #name); \
    tests_run++; \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
} while (0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAIL\n    Assertion failed: %s\n    at %s:%d\n", \
               #cond, __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAIL\n    Expected %lu, got %lu\n    at %s:%d\n", \
               (unsigned long)(b), (unsigned long)(a), __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

#define ASSERT_STR_EQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("FAIL\n    Expected \"%s\", got \"%s\"\n    at %s:%d\n", \
               (b), (a), __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

/* ========================================================================
 * Mock CAN bus
 * ====================================================================== */

#define MOCK_BUF_SIZE 64

static ms1_can_frame_t mock_tx_buf[MOCK_BUF_SIZE];
static int mock_tx_count = 0;

static ms1_can_frame_t mock_rx_buf[MOCK_BUF_SIZE];
static int mock_rx_count = 0;
static int mock_rx_read = 0;

static uint32_t mock_time_ms = 0;

static bool mock_can_send(const ms1_can_frame_t *frame)
{
    if (mock_tx_count < MOCK_BUF_SIZE) {
        mock_tx_buf[mock_tx_count++] = *frame;
        return true;
    }
    return false;
}

static bool mock_can_receive(ms1_can_frame_t *frame)
{
    if (mock_rx_read < mock_rx_count) {
        *frame = mock_rx_buf[mock_rx_read++];
        return true;
    }
    return false;
}

static uint32_t mock_millis(void)
{
    return mock_time_ms;
}

static void mock_reset(void)
{
    mock_tx_count = 0;
    mock_rx_count = 0;
    mock_rx_read = 0;
    mock_time_ms = 0;
}

static void mock_inject_frame(uint32_t id, uint8_t dlc, const uint8_t *data)
{
    if (mock_rx_count < MOCK_BUF_SIZE) {
        mock_rx_buf[mock_rx_count].id = id;
        mock_rx_buf[mock_rx_count].dlc = dlc;
        if (data && dlc > 0) {
            memcpy(mock_rx_buf[mock_rx_count].data, data, dlc);
        } else {
            memset(mock_rx_buf[mock_rx_count].data, 0, 8);
        }
        mock_rx_count++;
    }
}

static const ms1_platform_t mock_platform = {
    .can_send = mock_can_send,
    .can_receive = mock_can_receive,
    .millis = mock_millis,
    .log = NULL,
};

/* ========================================================================
 * Callback tracking
 * ====================================================================== */

static int cb_device_found_count = 0;
static int cb_device_lost_count = 0;
static int cb_lok_speed_count = 0;
static uint32_t cb_lok_speed_locid = 0;
static uint16_t cb_lok_speed_value = 0;
static int cb_lok_direction_count = 0;
static uint32_t cb_lok_dir_locid = 0;
static cs2_direction_t cb_lok_dir_value = CS2_DIR_KEEP;
static int cb_lok_function_count = 0;
static uint32_t cb_lok_func_locid = 0;
static uint8_t cb_lok_func_nr = 0;
static uint8_t cb_lok_func_value = 0;
static int cb_system_stop_count = 0;
static int cb_system_go_count = 0;
static int cb_system_halt_count = 0;
static int cb_overload_count = 0;
static uint8_t cb_overload_channel = 0;
static int cb_emergency_stop_count = 0;
static uint32_t cb_emergency_locid = 0;
static int cb_acc_switch_count = 0;
static int cb_s88_event_count = 0;
static int cb_config_request_count = 0;
static char cb_config_filename[16] = {0};
static int cb_config_received_count = 0;

static void cb_reset(void)
{
    cb_device_found_count = 0;
    cb_device_lost_count = 0;
    cb_lok_speed_count = 0;
    cb_lok_speed_locid = 0;
    cb_lok_speed_value = 0;
    cb_lok_direction_count = 0;
    cb_lok_dir_locid = 0;
    cb_lok_dir_value = CS2_DIR_KEEP;
    cb_lok_function_count = 0;
    cb_lok_func_locid = 0;
    cb_lok_func_nr = 0;
    cb_lok_func_value = 0;
    cb_system_stop_count = 0;
    cb_system_go_count = 0;
    cb_system_halt_count = 0;
    cb_overload_count = 0;
    cb_overload_channel = 0;
    cb_emergency_stop_count = 0;
    cb_emergency_locid = 0;
    cb_acc_switch_count = 0;
    cb_s88_event_count = 0;
    cb_config_request_count = 0;
    memset(cb_config_filename, 0, sizeof(cb_config_filename));
    cb_config_received_count = 0;
}

static void on_device_found(void *ctx, uint8_t idx, const cs2_device_t *dev)
{
    (void)ctx; (void)idx; (void)dev;
    cb_device_found_count++;
}

static void on_device_lost(void *ctx, uint8_t idx, const cs2_device_t *dev)
{
    (void)ctx; (void)idx; (void)dev;
    cb_device_lost_count++;
}

static void on_lok_speed(void *ctx, uint32_t locid, uint16_t speed)
{
    (void)ctx;
    cb_lok_speed_count++;
    cb_lok_speed_locid = locid;
    cb_lok_speed_value = speed;
}

static void on_lok_direction(void *ctx, uint32_t locid, cs2_direction_t dir)
{
    (void)ctx;
    cb_lok_direction_count++;
    cb_lok_dir_locid = locid;
    cb_lok_dir_value = dir;
}

static void on_lok_function(void *ctx, uint32_t locid, uint8_t func_nr,
                             uint8_t value)
{
    (void)ctx;
    cb_lok_function_count++;
    cb_lok_func_locid = locid;
    cb_lok_func_nr = func_nr;
    cb_lok_func_value = value;
}

static void on_system_stop(void *ctx) { (void)ctx; cb_system_stop_count++; }
static void on_system_go(void *ctx) { (void)ctx; cb_system_go_count++; }
static void on_system_halt(void *ctx) { (void)ctx; cb_system_halt_count++; }

static void on_overload(void *ctx, uint8_t channel)
{
    (void)ctx;
    cb_overload_count++;
    cb_overload_channel = channel;
}

static void on_emergency_stop(void *ctx, uint32_t locid)
{
    (void)ctx;
    cb_emergency_stop_count++;
    cb_emergency_locid = locid;
}

static void on_accessory_switch(void *ctx, uint32_t locid, uint8_t position,
                                 uint8_t power)
{
    (void)ctx; (void)locid; (void)position; (void)power;
    cb_acc_switch_count++;
}

static void on_s88_event(void *ctx, uint16_t device_id, uint16_t contact,
                          uint8_t old_state, uint8_t new_state)
{
    (void)ctx; (void)device_id; (void)contact;
    (void)old_state; (void)new_state;
    cb_s88_event_count++;
}

static void on_config_data_request(void *ctx, const char *filename)
{
    (void)ctx;
    cb_config_request_count++;
    strncpy(cb_config_filename, filename, sizeof(cb_config_filename) - 1);
}

static void on_config_data_received(void *ctx, const char *filename,
                                     const uint8_t *data, uint32_t length)
{
    (void)ctx; (void)filename; (void)data; (void)length;
    cb_config_received_count++;
}

static const cs2_master_callbacks_t test_callbacks = {
    .ctx = NULL,
    .on_device_found = on_device_found,
    .on_device_lost = on_device_lost,
    .on_lok_speed = on_lok_speed,
    .on_lok_direction = on_lok_direction,
    .on_lok_function = on_lok_function,
    .on_system_stop = on_system_stop,
    .on_system_go = on_system_go,
    .on_system_halt = on_system_halt,
    .on_overload = on_overload,
    .on_emergency_stop = on_emergency_stop,
    .on_accessory_switch = on_accessory_switch,
    .on_s88_event = on_s88_event,
    .on_config_data_request = on_config_data_request,
    .on_config_data_received = on_config_data_received,
    .on_update_offer = NULL,
};

/* ========================================================================
 * Helper: create initialized master for tests
 * ====================================================================== */

#define TEST_UID         0x43530001u
#define TEST_SW_VERSION  0x0301u
#define TEST_DEVICE_TYPE CS2_DEV_CS2_GUI

static cs2_master_t test_master;

static void setup_master(void)
{
    mock_reset();
    cb_reset();
    cs2_master_config_t cfg = {
        .uid = TEST_UID,
        .sw_version = TEST_SW_VERSION,
        .device_type = TEST_DEVICE_TYPE,
    };
    cs2_master_init(&test_master, &mock_platform, &cfg, &test_callbacks);
}

/* ========================================================================
 * Tests: CAN ID encode/decode
 * ====================================================================== */

TEST(test_can_id_roundtrip)
{
    cs2_can_id_t id = { .priority = 4, .command = 0x04, .response = 0, .hash = 0x4711 };
    uint32_t encoded = cs2_id_encode(&id);
    cs2_can_id_t decoded;
    cs2_id_decode(encoded, &decoded);
    ASSERT_EQ(decoded.priority, 4);
    ASSERT_EQ(decoded.command, 0x04);
    ASSERT_EQ(decoded.response, 0);
    ASSERT_EQ(decoded.hash, 0x4711);
}

TEST(test_can_id_bit_positions)
{
    /* Priority in bits 28..25 */
    cs2_can_id_t id = { .priority = 0x0F, .command = 0, .response = 0, .hash = 0 };
    ASSERT_EQ(cs2_id_encode(&id), 0x1E000000u);

    /* Command in bits 24..17 */
    cs2_can_id_t id2 = { .priority = 0, .command = 0xFF, .response = 0, .hash = 0 };
    ASSERT_EQ(cs2_id_encode(&id2), 0x01FE0000u);

    /* Response at bit 16 */
    cs2_can_id_t id3 = { .priority = 0, .command = 0, .response = 1, .hash = 0 };
    ASSERT_EQ(cs2_id_encode(&id3), 0x00010000u);

    /* Hash in bits 15..0 */
    cs2_can_id_t id4 = { .priority = 0, .command = 0, .response = 0, .hash = 0xFFFF };
    ASSERT_EQ(cs2_id_encode(&id4), 0x0000FFFFu);
}

TEST(test_can_id_spec_example)
{
    /* Spec example: CAN ID 0x00084711 = prio=0, cmd=0x04 (Lok Speed), resp=0, hash=0x4711 */
    cs2_can_id_t decoded;
    cs2_id_decode(0x00084711u, &decoded);
    ASSERT_EQ(decoded.priority, 0);
    ASSERT_EQ(decoded.command, 0x04);
    ASSERT_EQ(decoded.response, 0);
    ASSERT_EQ(decoded.hash, 0x4711);

    /* Verify re-encoding */
    ASSERT_EQ(cs2_id_encode(&decoded), 0x00084711u);
}

TEST(test_can_id_response_bit)
{
    /* Request: response=0 */
    cs2_can_id_t req = { .priority = 0, .command = 0x04, .response = 0, .hash = 0x4711 };
    uint32_t req_id = cs2_id_encode(&req);

    /* Response: response=1 */
    cs2_can_id_t resp = { .priority = 0, .command = 0x04, .response = 1, .hash = 0x4711 };
    uint32_t resp_id = cs2_id_encode(&resp);

    /* Difference should be exactly bit 16 */
    ASSERT_EQ(resp_id - req_id, 0x00010000u);

    /* Decode response */
    cs2_can_id_t decoded;
    cs2_id_decode(resp_id, &decoded);
    ASSERT_EQ(decoded.response, 1);
    ASSERT_EQ(decoded.command, 0x04);
}

/* ========================================================================
 * Tests: Hash calculation
 * ====================================================================== */

TEST(test_hash_from_uid)
{
    /* Test that bits 9,8,7 are always 1,1,0 (value 6 << 7 = 0x0300) */
    uint16_t hash = cs2_hash_from_uid(0x12345678u);
    /* XOR: 0x1234 ^ 0x5678 = 0x444C */
    /* Clear bits 7-9: 0x444C & 0xFC7F = 0x444C (bits 7-9 of 0x444C = 0b001 -> bit8=0,bit7=0) */
    /* Set bits 8-9: | 0x0300 */
    /* Verify bits 7-9 = 0b110 */
    ASSERT_EQ((hash >> 7) & 0x07, 0x06);
}

TEST(test_hash_edge_cases)
{
    /* UID = 0 */
    uint16_t hash0 = cs2_hash_from_uid(0x00000000u);
    /* XOR: 0 ^ 0 = 0, then set bits => 0x0300 */
    ASSERT_EQ((hash0 >> 7) & 0x07, 0x06);
    ASSERT_EQ(hash0, 0x0300u);

    /* UID = 0xFFFFFFFF */
    uint16_t hashF = cs2_hash_from_uid(0xFFFFFFFFu);
    /* XOR: 0xFFFF ^ 0xFFFF = 0x0000, then fix bits => 0x0300 */
    ASSERT_EQ((hashF >> 7) & 0x07, 0x06);
    ASSERT_EQ(hashF, 0x0300u);
}

/* ========================================================================
 * Tests: CRC-16
 * ====================================================================== */

TEST(test_crc16)
{
    /* Known test vector: "123456789" */
    const uint8_t data[] = "123456789";
    uint16_t crc = cs2_crc16(data, 9);
    /* CRC-16/CCITT-FALSE for "123456789" = 0x29B1 */
    ASSERT_EQ(crc, 0x29B1u);
}

/* ========================================================================
 * Tests: Loc-ID encode/decode
 * ====================================================================== */

TEST(test_locid_encode_mm)
{
    /* MM address 72 → Loc-ID 0x0048 */
    uint32_t locid = cs2_locid_encode(72, CS2_PROTO_MM);
    ASSERT_EQ(locid, 0x0048u);
    ASSERT_EQ(cs2_locid_protocol(locid), CS2_PROTO_MM);
    ASSERT_EQ(cs2_locid_address(locid), 72);
}

TEST(test_locid_encode_dcc)
{
    /* DCC address 3 → Loc-ID 0xC003 */
    uint32_t locid = cs2_locid_encode(3, CS2_PROTO_DCC);
    ASSERT_EQ(locid, 0xC003u);
    ASSERT_EQ(cs2_locid_protocol(locid), CS2_PROTO_DCC);
    ASSERT_EQ(cs2_locid_address(locid), 3);
}

TEST(test_locid_encode_mfx)
{
    /* MFX SID 5 → Loc-ID 0x4005 */
    uint32_t locid = cs2_locid_encode(5, CS2_PROTO_MFX);
    ASSERT_EQ(locid, 0x4005u);
    ASSERT_EQ(cs2_locid_protocol(locid), CS2_PROTO_MFX);
    ASSERT_EQ(cs2_locid_address(locid), 5);
}

TEST(test_locid_roundtrip)
{
    cs2_protocol_t protos[] = { CS2_PROTO_MM, CS2_PROTO_MFX, CS2_PROTO_DCC, CS2_PROTO_SX1 };
    uint16_t addrs[] = { 1, 100, 255 };
    for (int p = 0; p < 4; p++) {
        for (int a = 0; a < 3; a++) {
            uint32_t locid = cs2_locid_encode(addrs[a], protos[p]);
            ASSERT_EQ(cs2_locid_protocol(locid), protos[p]);
            ASSERT_EQ(cs2_locid_address(locid), addrs[a]);
        }
    }
}

/* ========================================================================
 * Tests: Master init
 * ====================================================================== */

TEST(test_master_init)
{
    setup_master();
    ASSERT_EQ(test_master.state, CS2_STATE_IDLE);
    ASSERT_EQ(test_master.hash, cs2_hash_from_uid(TEST_UID));
    ASSERT(test_master.stopped);
    ASSERT(!test_master.halt);
    /* All device/lok/acc slots should be empty */
    for (int i = 0; i < CS2_MAX_DEVICES; i++) {
        ASSERT(!test_master.devices[i].present);
    }
    for (int i = 0; i < CS2_MAX_LOKS; i++) {
        ASSERT(!test_master.loks[i].active);
    }
}

/* ========================================================================
 * Tests: Ping
 * ====================================================================== */

TEST(test_ping_query)
{
    setup_master();

    /* Inject a DLC=0 ping query from another device */
    cs2_can_id_t ping_id = { .priority = 4, .command = CS2_CMD_PING,
                              .response = 0, .hash = 0x1234 };
    mock_inject_frame(cs2_id_encode(&ping_id), 0, NULL);

    cs2_master_poll(&test_master);

    /* Master should respond with DLC=8 ping response */
    ASSERT(mock_tx_count >= 1);
    /* Find the ping response */
    bool found = false;
    for (int i = 0; i < mock_tx_count; i++) {
        cs2_can_id_t tid;
        cs2_id_decode(mock_tx_buf[i].id, &tid);
        if (tid.command == CS2_CMD_PING && tid.response == 1) {
            ASSERT_EQ(mock_tx_buf[i].dlc, 8);
            /* Check UID in data[0..3] */
            uint32_t uid = ms1_data_get_u32(mock_tx_buf[i].data, 0);
            ASSERT_EQ(uid, TEST_UID);
            /* Check SW version in data[4..5] */
            uint16_t sw = ms1_data_get_u16(mock_tx_buf[i].data, 4);
            ASSERT_EQ(sw, TEST_SW_VERSION);
            /* Check device type in data[6..7] */
            uint16_t dt = ms1_data_get_u16(mock_tx_buf[i].data, 6);
            ASSERT_EQ(dt, (uint16_t)TEST_DEVICE_TYPE);
            found = true;
            break;
        }
    }
    ASSERT(found);
}

TEST(test_ping_response_registers_device)
{
    setup_master();

    /* Inject a DLC=8 ping response (a device announcing itself) */
    uint8_t data[8];
    uint32_t ms2_uid = 0x4D533201u;
    ms1_data_put_u32(data, 0, ms2_uid);
    ms1_data_put_u16(data, 4, 0x0200);  /* SW version 2.00 */
    ms1_data_put_u16(data, 6, CS2_DEV_MS2);

    cs2_can_id_t resp_id = { .priority = 4, .command = CS2_CMD_PING,
                              .response = 1, .hash = 0xABCD };
    mock_inject_frame(cs2_id_encode(&resp_id), 8, data);

    cs2_master_poll(&test_master);

    /* Device should be registered */
    ASSERT_EQ(cb_device_found_count, 1);
    cs2_device_t dev;
    ASSERT(cs2_master_get_device(&test_master, 0, &dev));
    ASSERT_EQ(dev.uid, ms2_uid);
    ASSERT_EQ(dev.sw_version, 0x0200);
    ASSERT_EQ(dev.device_type, (uint16_t)CS2_DEV_MS2);
    ASSERT_EQ(dev.hash, 0xABCD);
}

/* ========================================================================
 * Tests: System commands
 * ====================================================================== */

TEST(test_system_stop_send)
{
    setup_master();
    mock_tx_count = 0;

    cs2_master_system_stop(&test_master);

    ASSERT_EQ(mock_tx_count, 1);
    cs2_can_id_t tid;
    cs2_id_decode(mock_tx_buf[0].id, &tid);
    ASSERT_EQ(tid.command, CS2_CMD_SYSTEM);
    ASSERT_EQ(tid.response, 0);
    ASSERT_EQ(mock_tx_buf[0].dlc, 5);
    /* data[0..3] = broadcast UID (0) */
    ASSERT_EQ(ms1_data_get_u32(mock_tx_buf[0].data, 0), CS2_UID_BROADCAST);
    /* data[4] = SYS_STOP sub-command */
    ASSERT_EQ(mock_tx_buf[0].data[4], CS2_SYS_STOP);
    ASSERT(test_master.stopped);
}

TEST(test_system_go_send)
{
    setup_master();
    mock_tx_count = 0;

    cs2_master_system_go(&test_master);

    ASSERT_EQ(mock_tx_count, 1);
    cs2_can_id_t tid;
    cs2_id_decode(mock_tx_buf[0].id, &tid);
    ASSERT_EQ(tid.command, CS2_CMD_SYSTEM);
    ASSERT_EQ(mock_tx_buf[0].data[4], CS2_SYS_GO);
    ASSERT(!test_master.stopped);
    ASSERT_EQ(test_master.state, CS2_STATE_RUNNING);
}

TEST(test_system_stop_receive)
{
    setup_master();
    test_master.state = CS2_STATE_RUNNING;
    test_master.stopped = false;

    /* Inject a system stop request from another device */
    uint8_t data[5];
    ms1_data_put_u32(data, 0, CS2_UID_BROADCAST);
    data[4] = CS2_SYS_STOP;

    cs2_can_id_t cmd_id = { .priority = CS2_PRIO_STOP_GO, .command = CS2_CMD_SYSTEM,
                             .response = 0, .hash = 0x1234 };
    mock_inject_frame(cs2_id_encode(&cmd_id), 5, data);

    cs2_master_poll(&test_master);

    ASSERT_EQ(cb_system_stop_count, 1);
    ASSERT(test_master.stopped);
    ASSERT_EQ(test_master.state, CS2_STATE_STOPPED);
}

/* ========================================================================
 * Tests: Lok commands
 * ====================================================================== */

TEST(test_lok_speed_send)
{
    setup_master();

    /* Add a lok */
    cs2_lok_t lok = {0};
    strcpy(lok.name, "BR101");
    lok.locid = cs2_locid_encode(72, CS2_PROTO_MM);
    lok.protocol = CS2_PROTO_MM;
    lok.address = 72;
    lok.speed = 0;
    lok.direction = CS2_DIR_FORWARD;
    cs2_master_set_lok(&test_master, 0, &lok);

    mock_tx_count = 0;
    cs2_master_lok_speed(&test_master, lok.locid, 500);

    ASSERT_EQ(mock_tx_count, 1);
    cs2_can_id_t tid;
    cs2_id_decode(mock_tx_buf[0].id, &tid);
    ASSERT_EQ(tid.command, CS2_CMD_LOK_SPEED);
    ASSERT_EQ(tid.response, 0);
    ASSERT_EQ(mock_tx_buf[0].dlc, 6);
    /* data[0..3] = Loc-ID */
    ASSERT_EQ(ms1_data_get_u32(mock_tx_buf[0].data, 0), lok.locid);
    /* data[4..5] = speed */
    ASSERT_EQ(ms1_data_get_u16(mock_tx_buf[0].data, 4), 500);
    /* Internal state updated */
    ASSERT_EQ(test_master.loks[0].speed, 500);
}

TEST(test_lok_direction_send)
{
    setup_master();

    cs2_lok_t lok = {0};
    strcpy(lok.name, "ICE3");
    lok.locid = cs2_locid_encode(3, CS2_PROTO_DCC);
    lok.protocol = CS2_PROTO_DCC;
    lok.address = 3;
    lok.direction = CS2_DIR_FORWARD;
    cs2_master_set_lok(&test_master, 0, &lok);

    mock_tx_count = 0;
    cs2_master_lok_direction(&test_master, lok.locid, CS2_DIR_BACKWARD);

    ASSERT_EQ(mock_tx_count, 1);
    cs2_can_id_t tid;
    cs2_id_decode(mock_tx_buf[0].id, &tid);
    ASSERT_EQ(tid.command, CS2_CMD_LOK_DIRECTION);
    ASSERT_EQ(mock_tx_buf[0].dlc, 5);
    ASSERT_EQ(ms1_data_get_u32(mock_tx_buf[0].data, 0), lok.locid);
    ASSERT_EQ(mock_tx_buf[0].data[4], CS2_DIR_BACKWARD);
    /* State updated */
    ASSERT_EQ(test_master.loks[0].direction, CS2_DIR_BACKWARD);
}

TEST(test_lok_function_receive)
{
    setup_master();

    cs2_lok_t lok = {0};
    strcpy(lok.name, "V60");
    lok.locid = cs2_locid_encode(60, CS2_PROTO_MM);
    lok.protocol = CS2_PROTO_MM;
    lok.address = 60;
    lok.func_count = 8;
    cs2_master_set_lok(&test_master, 0, &lok);

    /* Inject a function set command (F3=1) from MS2 */
    uint8_t data[6];
    ms1_data_put_u32(data, 0, lok.locid);
    data[4] = 3;  /* F3 */
    data[5] = 1;  /* ON */

    cs2_can_id_t func_id = { .priority = CS2_PRIO_LOK_ACC,
                              .command = CS2_CMD_LOK_FUNCTION,
                              .response = 0, .hash = 0x5678 };
    mock_inject_frame(cs2_id_encode(&func_id), 6, data);

    cs2_master_poll(&test_master);

    /* Callback should fire */
    ASSERT_EQ(cb_lok_function_count, 1);
    ASSERT_EQ(cb_lok_func_locid, lok.locid);
    ASSERT_EQ(cb_lok_func_nr, 3);
    ASSERT_EQ(cb_lok_func_value, 1);

    /* Internal state updated */
    ASSERT(test_master.loks[0].functions & (1u << 3));

    /* Master should have sent a response */
    bool found_resp = false;
    for (int i = 0; i < mock_tx_count; i++) {
        cs2_can_id_t tid;
        cs2_id_decode(mock_tx_buf[i].id, &tid);
        if (tid.command == CS2_CMD_LOK_FUNCTION && tid.response == 1) {
            found_resp = true;
            break;
        }
    }
    ASSERT(found_resp);
}

/* ========================================================================
 * Tests: Accessory
 * ====================================================================== */

TEST(test_acc_switch_send)
{
    setup_master();
    mock_tx_count = 0;

    uint32_t acc_locid = cs2_locid_encode(1, CS2_PROTO_MM) + CS2_LOCID_MM_ACC_BASE;
    cs2_master_acc_switch(&test_master, acc_locid, 1, 1);

    ASSERT_EQ(mock_tx_count, 1);
    cs2_can_id_t tid;
    cs2_id_decode(mock_tx_buf[0].id, &tid);
    ASSERT_EQ(tid.command, CS2_CMD_ACC_SWITCH);
    ASSERT_EQ(tid.response, 0);
    ASSERT_EQ(mock_tx_buf[0].dlc, 6);
    ASSERT_EQ(ms1_data_get_u32(mock_tx_buf[0].data, 0), acc_locid);
    ASSERT_EQ(mock_tx_buf[0].data[4], 1);  /* position */
    ASSERT_EQ(mock_tx_buf[0].data[5], 1);  /* power */
}

/* ========================================================================
 * Tests: Config data
 * ====================================================================== */

TEST(test_config_data_request)
{
    setup_master();

    /* Inject a config data request for "lokinfo" */
    uint8_t data[8] = { 'l', 'o', 'k', 'i', 'n', 'f', 'o', '\0' };
    cs2_can_id_t req_id = { .priority = CS2_PRIO_LOK_ACC,
                             .command = CS2_CMD_CFG_DATA_REQ,
                             .response = 0, .hash = 0x9876 };
    mock_inject_frame(cs2_id_encode(&req_id), 8, data);

    cs2_master_poll(&test_master);

    ASSERT_EQ(cb_config_request_count, 1);
    ASSERT_STR_EQ(cb_config_filename, "lokinfo");
    ASSERT_EQ(test_master.config_stream.peer_hash, 0x9876);
}

TEST(test_config_data_provide)
{
    setup_master();

    /* First, simulate a config data request to set peer_hash */
    test_master.config_stream.peer_hash = 0x9876;

    /* Provide some test data */
    const uint8_t test_data[] = "Hello, CS2 config stream!";
    uint32_t len = (uint32_t)strlen((const char *)test_data);

    mock_tx_count = 0;
    bool ok = cs2_master_config_provide(&test_master, "lokinfo",
                                         test_data, len);
    ASSERT(ok);

    /* Should have sent header packet (DLC=6) */
    ASSERT(mock_tx_count >= 1);
    cs2_can_id_t hid;
    cs2_id_decode(mock_tx_buf[0].id, &hid);
    ASSERT_EQ(hid.command, CS2_CMD_CFG_DATA_STREAM);
    ASSERT_EQ(hid.response, 1);
    ASSERT_EQ(hid.hash, 0x9876);
    ASSERT_EQ(mock_tx_buf[0].dlc, 6);

    /* Header data: length(4) + CRC(2) */
    uint32_t hdr_len = ms1_data_get_u32(mock_tx_buf[0].data, 0);
    ASSERT_EQ(hdr_len, len);
    uint16_t hdr_crc = ms1_data_get_u16(mock_tx_buf[0].data, 4);
    ASSERT_EQ(hdr_crc, cs2_crc16(test_data, len));

    /* Stream state should be SENDING */
    ASSERT_EQ(test_master.config_stream.state, CS2_CFG_SENDING);

    /* Poll should send data packets */
    mock_tx_count = 0;
    cs2_master_poll(&test_master);

    /* Should have sent data packets */
    ASSERT(mock_tx_count > 0);
    /* After streaming is complete, state should return to IDLE */
    /* (data is 25 bytes = 4 packets of 8 bytes, fits in one tick of 4 packets) */
    ASSERT_EQ(test_master.config_stream.state, CS2_CFG_IDLE);
}

/* ========================================================================
 * Main
 * ====================================================================== */

int main(void)
{
    printf("CS2 CAN Protocol Tests\n");
    printf("======================\n\n");

    printf("CAN ID encode/decode:\n");
    RUN_TEST(test_can_id_roundtrip);
    RUN_TEST(test_can_id_bit_positions);
    RUN_TEST(test_can_id_spec_example);
    RUN_TEST(test_can_id_response_bit);

    printf("\nHash calculation:\n");
    RUN_TEST(test_hash_from_uid);
    RUN_TEST(test_hash_edge_cases);

    printf("\nCRC-16:\n");
    RUN_TEST(test_crc16);

    printf("\nLoc-ID:\n");
    RUN_TEST(test_locid_encode_mm);
    RUN_TEST(test_locid_encode_dcc);
    RUN_TEST(test_locid_encode_mfx);
    RUN_TEST(test_locid_roundtrip);

    printf("\nMaster init:\n");
    RUN_TEST(test_master_init);

    printf("\nPing:\n");
    RUN_TEST(test_ping_query);
    RUN_TEST(test_ping_response_registers_device);

    printf("\nSystem commands:\n");
    RUN_TEST(test_system_stop_send);
    RUN_TEST(test_system_go_send);
    RUN_TEST(test_system_stop_receive);

    printf("\nLok commands:\n");
    RUN_TEST(test_lok_speed_send);
    RUN_TEST(test_lok_direction_send);
    RUN_TEST(test_lok_function_receive);

    printf("\nAccessory:\n");
    RUN_TEST(test_acc_switch_send);

    printf("\nConfig data:\n");
    RUN_TEST(test_config_data_request);
    RUN_TEST(test_config_data_provide);

    printf("\n======================\n");
    printf("Results: %d/%d tests passed\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
