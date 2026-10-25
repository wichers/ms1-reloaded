/**
 * @file ms1_test_trace.c
 * @brief Tests for M-CAN protocol implementation using real CAN trace data.
 *
 * Test data sourced from CAN_Doku_V101_de.pdf Chapter 11 (pages 53-56):
 * Real telegram trace of an MS1 connecting to an ECoS (FW 3.0.1) with
 * Lok 003 131-0 in the lokstack, and the frames of chapters 6-7.
 *
 * Trace table columns map to operational CAN ID format:
 *   Priority(3) | Object-Handle(16) | Command(3) | Node(7) | DLC | Data[0..7]
 *
 * For registration frames (node=126) the same layout carries
 * uid_byte(8) | stage(3) | MID(5) in the handle and the direction in the
 * command field (0 = slave, 1 = master).
 *
 * The master integration tests at the end run the library against frames an
 * MS1 slave sends (as the original firmware does, see ms1_test_reference.c).
 *
 * Build: make test
 */

#include "ms1_types.h"
#include "ms1_master.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ========================================================================
 * Test counters
 * ====================================================================== */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST_BEGIN(name) do { \
    tests_run++; \
    printf("  [TEST] %-50s ", name); \
} while(0)

#define TEST_PASS() do { \
    tests_passed++; \
    printf("PASS\n"); \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAIL\n    %s:%d: %s = 0x%lX, expected 0x%lX\n", \
               __FILE__, __LINE__, #a, (unsigned long)(a), (unsigned long)(b)); \
        return; \
    } \
} while(0)

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return; \
    } \
} while(0)

#define ASSERT_MEM_EQ(a, b, len) do { \
    if (memcmp((a), (b), (len)) != 0) { \
        printf("FAIL\n    %s:%d: memory mismatch\n", __FILE__, __LINE__); \
        return; \
    } \
} while(0)

/* ========================================================================
 * Helper: build 29-bit CAN ID from trace table columns
 * ====================================================================== */

static uint32_t trace_can_id(uint8_t prio, uint16_t handle, uint8_t cmd, uint8_t node)
{
    return ((uint32_t)(prio & 7) << 26)
         | ((uint32_t)(handle & 0xFFFF) << 10)
         | ((uint32_t)(cmd & 7) << 7)
         | (node & 0x7F);
}

/* ========================================================================
 * Trace constants from the real MS1 <-> ECoS session
 * ====================================================================== */

/* ECoS assigned nodes: master=42 (0x2A), slave=43 (0x2B) */
#define TRACE_MASTER_NODE   42
#define TRACE_SLAVE_NODE    43

/* The slave's UID: its stage-4 data and the ECoS's stage-7 answer, 00 02 04 B3.
 * The ECoS's own UID: its stage-0 answer and its monitoring answers, 00 04 84 D3. */
static const uint8_t TRACE_SLAVE_UID[4]  = {0x00, 0x02, 0x04, 0xB3};
static const uint8_t TRACE_MASTER_UID[4] = {0x00, 0x04, 0x84, 0xD3};

/* Handles from the trace: monitoring (stage-0 answer data[6..7]), start object
 * (stage-7 answer data[4..5]), SD (the slave's own object), SY (status) */
#define TRACE_MONITOR       0x0001
#define TRACE_START_OBJ     0x0002
#define TRACE_SD_HANDLE     0x0008
#define TRACE_SY_HANDLE     0x0003

/* ========================================================================
 * Test 1: Bus-occupied frame constant
 *
 * The bus-occupied probe frame has a fixed CAN ID of 0x380.
 * Decoded as operational: prio=0, handle=0, cmd=MONITOR(7), node=0
 * ====================================================================== */

static void test_bus_occupied_id(void)
{
    TEST_BEGIN("bus_occupied_id");

    /* Verify the constant */
    ASSERT_EQ(MS1_ID_BUS_OCCUPIED, 0x00000380u);

    /* Verify it encodes correctly from operational fields */
    ms1_op_id_t oid;
    oid.priority = 0;
    oid.handle = 0;
    oid.command = MS1_CMD_MONITOR;
    oid.node = 0;
    ASSERT_EQ(ms1_id_encode_op(&oid), MS1_ID_BUS_OCCUPIED);

    /* Verify decode */
    ms1_op_id_t dec;
    ms1_id_decode_op(MS1_ID_BUS_OCCUPIED, &dec);
    ASSERT_EQ(dec.priority, 0);
    ASSERT_EQ(dec.handle, 0);
    ASSERT_EQ(dec.command, MS1_CMD_MONITOR);
    ASSERT_EQ(dec.node, 0);

    TEST_PASS();
}

/* ========================================================================
 * Test 2: Operational CAN ID encode/decode roundtrip
 * ====================================================================== */

static void test_op_id_roundtrip(void)
{
    TEST_BEGIN("op_id_roundtrip");

    /* Test various priority/handle/command/node combinations */
    struct {
        uint8_t prio; uint16_t handle; uint8_t cmd; uint8_t node;
    } cases[] = {
        { 0, 0x0000, 0, 0 },
        { 7, 0xFFFF, 7, 127 },
        { 3, 0x0001, 7, 43 },  /* heartbeat slave */
        { 3, 0x0001, 7, 42 },  /* heartbeat master */
        { 6, 0x0002, 4, 43 },  /* SD-handle request */
        { 6, 0x0008, 1, 42 },  /* name on SD-handle */
        { 6, 0x0003, 2, 43 },  /* SY status query */
        { 5, 0x0001, 0, 42 },  /* assignment */
        { 1, 0x0001, 2, 43 },  /* slave drives lok */
        { 2, 0x0001, 2, 42 },  /* master drives lok */
    };

    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        ms1_op_id_t oid;
        oid.priority = cases[i].prio;
        oid.handle = cases[i].handle;
        oid.command = cases[i].cmd;
        oid.node = cases[i].node;

        uint32_t encoded = ms1_id_encode_op(&oid);
        ms1_op_id_t dec;
        ms1_id_decode_op(encoded, &dec);

        ASSERT_EQ(dec.priority, cases[i].prio);
        ASSERT_EQ(dec.handle, cases[i].handle);
        ASSERT_EQ(dec.command, cases[i].cmd);
        ASSERT_EQ(dec.node, cases[i].node);
    }

    TEST_PASS();
}

/* ========================================================================
 * Test 3: Registration CAN ID encode/decode roundtrip
 * ====================================================================== */

static void test_reg_id_roundtrip(void)
{
    TEST_BEGIN("reg_id_roundtrip");

    /* Registration uses operational layout with fields packed in handle:
     * handle = (uid_byte << 8) | (stage << 5) | direction */
    struct {
        uint8_t prio; uint8_t uid_byte; uint8_t stage; uint8_t dir; uint8_t node; uint8_t mid;
    } cases[] = {
        { 7, 0x00, 0, 0, 126,  0 },   /* stage 0 from slave, uid_byte=0 */
        { 7, 0x00, 0, 1, 126, 12 },   /* stage 0 master response, MID 12 */
        { 7, 0xFF, 7, 1, 126, 31 },   /* max values */
        { 7, 0x52, 2, 0, 126,  5 },   /* stage 2 with uid byte */
        { 7, 0x00, 5, 1, 126,  1 },   /* stage 5 master (no uid byte) */
    };

    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        ms1_reg_id_t rid;
        rid.priority = cases[i].prio;
        rid.uid_byte = cases[i].uid_byte;
        rid.stage = cases[i].stage;
        rid.direction = cases[i].dir;
        rid.node = cases[i].node;
        rid.mid = cases[i].mid;

        uint32_t encoded = ms1_id_encode_reg(&rid);
        ms1_reg_id_t dec;
        ms1_id_decode_reg(encoded, &dec);

        ASSERT_EQ(dec.priority, cases[i].prio);
        ASSERT_EQ(dec.uid_byte, cases[i].uid_byte);
        ASSERT_EQ(dec.stage, cases[i].stage);
        ASSERT_EQ(dec.direction, cases[i].dir);
        ASSERT_EQ(dec.node, cases[i].node);
        ASSERT_EQ(dec.mid, cases[i].mid);
    }

    TEST_PASS();
}

/* ========================================================================
 * Test 4: Flash CAN ID encode/decode roundtrip
 * ====================================================================== */

static void test_flash_id_roundtrip(void)
{
    TEST_BEGIN("flash_id_roundtrip");

    struct {
        uint8_t prio; uint8_t fh; uint8_t req; uint8_t stage; uint8_t flags; uint8_t node;
    } cases[] = {
        { 7, 0x00, 0, 0, 0, 127 },     /* flash readiness signal */
        { 7, 0x02, 0, 0, 0, 127 },     /* flash handle assigned */
        { 7, 0xFF, 7, 31, 7, 127 },    /* max values */
        { 7, 0x04, 1, 5, 2, 127 },     /* data transfer */
    };

    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        ms1_flash_id_t fid;
        fid.priority = cases[i].prio;
        fid.flash_handle = cases[i].fh;
        fid.request = cases[i].req;
        fid.stage = cases[i].stage;
        fid.flags = cases[i].flags;
        fid.node = cases[i].node;

        uint32_t encoded = ms1_id_encode_flash(&fid);
        ms1_flash_id_t dec;
        ms1_id_decode_flash(encoded, &dec);

        ASSERT_EQ(dec.priority, cases[i].prio);
        ASSERT_EQ(dec.flash_handle, cases[i].fh);
        ASSERT_EQ(dec.request, cases[i].req);
        ASSERT_EQ(dec.stage, cases[i].stage);
        ASSERT_EQ(dec.flags, cases[i].flags);
        ASSERT_EQ(dec.node, cases[i].node);
    }

    TEST_PASS();
}

/* ========================================================================
 * Test 5: Data byte helpers
 * ====================================================================== */

static void test_data_helpers(void)
{
    TEST_BEGIN("data_helpers");

    uint8_t buf[8] = {0};

    /* u16 */
    ms1_data_put_u16(buf, 0, 0x1234);
    ASSERT_EQ(buf[0], 0x12);
    ASSERT_EQ(buf[1], 0x34);
    ASSERT_EQ(ms1_data_get_u16(buf, 0), 0x1234);

    ms1_data_put_u16(buf, 4, 0x0008); /* SD-Handle value */
    ASSERT_EQ(buf[4], 0x00);
    ASSERT_EQ(buf[5], 0x08);
    ASSERT_EQ(ms1_data_get_u16(buf, 4), 0x0008);

    /* u32 */
    ms1_data_put_u32(buf, 0, 0xDEADBEEF);
    ASSERT_EQ(buf[0], 0xDE);
    ASSERT_EQ(buf[1], 0xAD);
    ASSERT_EQ(buf[2], 0xBE);
    ASSERT_EQ(buf[3], 0xEF);
    ASSERT_EQ(ms1_data_get_u32(buf, 0), 0xDEADBEEF);

    /* The ECoS's UID from the trace */
    ms1_data_put_u32(buf, 0, 0x000484D3);
    ASSERT_MEM_EQ(buf, TRACE_MASTER_UID, 4);
    ASSERT_EQ(ms1_data_get_u32(buf, 0), 0x000484D3);

    TEST_PASS();
}

/* ========================================================================
 * Test 6: Trace - Registration stage 0 from slave
 *
 * From page 53, row 3:
 *   111 | 00000000 00000000 | 000 | 1111110 | DLC=0
 * ====================================================================== */

static void test_trace_reg_stage0(void)
{
    TEST_BEGIN("trace_reg_stage0");

    /* Build the CAN ID from trace table columns */
    uint32_t can_id = trace_can_id(7, 0x0000, 0, 126);
    ASSERT_EQ(can_id, 0x1C00007E);

    /* Decode as registration */
    ms1_reg_id_t rid;
    ms1_id_decode_reg(can_id, &rid);
    ASSERT_EQ(rid.priority, MS1_PRIO_REG_FLASH);
    ASSERT_EQ(rid.uid_byte, 0);
    ASSERT_EQ(rid.stage, MS1_REG_STAGE_UID3);  /* Stage 0 */
    ASSERT_EQ(rid.direction, MS1_DIR_FROM_SLAVE);
    ASSERT_EQ(rid.node, MS1_NODE_REG);

    /* Verify re-encoding gives back the same ID */
    ASSERT_EQ(ms1_id_encode_reg(&rid), can_id);

    TEST_PASS();
}

/* ========================================================================
 * Test 7: Trace - Heartbeat exchange
 *
 * From page 56:
 *   Slave heartbeat:
 *     011 | 00000000 00000001 | 111 | 0101011 | DLC=4 | 00 00 00 2A
 *   Master heartbeat response (the master's own UID, not the slave's as
 *   section 7.2.1 of the document says; data[0] is 0x00, not 0x03):
 *     011 | 00000000 00000001 | 111 | 0101010 | DLC=8 | 00 00 00 2A 00 04 84 D3
 * ====================================================================== */

static void test_trace_heartbeat(void)
{
    TEST_BEGIN("trace_heartbeat");

    /* --- Slave heartbeat (from node 43) --- */
    uint32_t slave_hb_id = trace_can_id(3, 0x0001, 7, TRACE_SLAVE_NODE);
    ASSERT_EQ(slave_hb_id, 0x0C0007AB);

    ms1_op_id_t dec;
    ms1_id_decode_op(slave_hb_id, &dec);
    ASSERT_EQ(dec.priority, MS1_PRIO_HEARTBEAT);
    ASSERT_EQ(dec.handle, TRACE_MONITOR);
    ASSERT_EQ(dec.command, MS1_CMD_MONITOR);
    ASSERT_EQ(dec.node, TRACE_SLAVE_NODE);

    /* Slave heartbeat data: Data[3] = master node (Knot) */
    uint8_t slave_hb_data[4] = {0x00, 0x00, 0x00, 0x2A};
    ASSERT_EQ(slave_hb_data[3], TRACE_MASTER_NODE);

    /* --- Master heartbeat response (from node 42) --- */
    uint32_t master_hb_id = trace_can_id(3, 0x0001, 7, TRACE_MASTER_NODE);
    ASSERT_EQ(master_hb_id, 0x0C0007AA);

    ms1_id_decode_op(master_hb_id, &dec);
    ASSERT_EQ(dec.priority, MS1_PRIO_HEARTBEAT);
    ASSERT_EQ(dec.handle, TRACE_MONITOR);
    ASSERT_EQ(dec.command, MS1_CMD_MONITOR);
    ASSERT_EQ(dec.node, TRACE_MASTER_NODE);

    /* Master heartbeat data: Data[3]=Knot(42), Data[4..7]=master UID */
    uint8_t master_hb_data[8] = {0x00, 0x00, 0x00, 0x2A, 0x00, 0x04, 0x84, 0xD3};
    ASSERT_EQ(master_hb_data[3], TRACE_MASTER_NODE);
    ASSERT_MEM_EQ(&master_hb_data[4], TRACE_MASTER_UID, 4);

    TEST_PASS();
}

/* ========================================================================
 * Test 8: Trace - SD-Handle request and response
 *
 * From page 53 (bottom rows):
 *   Request from slave (node=43):
 *     110 | 00000000 00000010 | 100 | 0101011 | DLC=2 | 40 2B
 *   Response from master (node=42):
 *     110 | 00000000 00000010 | 100 | 0101010 | DLC=8 | 40 2B 00 00 00 08 00 01
 * ====================================================================== */

static void test_trace_sd_handle(void)
{
    TEST_BEGIN("trace_sd_handle_request");

    /* --- SD-Handle request from slave --- */
    uint32_t req_id = trace_can_id(6, 0x0002, 4, TRACE_SLAVE_NODE);

    ms1_op_id_t dec;
    ms1_id_decode_op(req_id, &dec);
    ASSERT_EQ(dec.priority, MS1_PRIO_DATA);
    ASSERT_EQ(dec.handle, TRACE_START_OBJ); /* from the stage-7 answer */
    ASSERT_EQ(dec.command, MS1_CMD_OBJECT2);
    ASSERT_EQ(dec.node, TRACE_SLAVE_NODE);

    /* Request data: marker=0x40 (SD_REQUEST), slave node */
    uint8_t req_data[2] = {MS1_DATA_SD_REQUEST, TRACE_SLAVE_NODE};
    ASSERT_EQ(req_data[0], 0x40);
    ASSERT_EQ(req_data[1], 0x2B);

    /* --- SD-Handle response from master --- */
    uint32_t resp_id = trace_can_id(6, 0x0002, 4, TRACE_MASTER_NODE);

    ms1_id_decode_op(resp_id, &dec);
    ASSERT_EQ(dec.priority, MS1_PRIO_DATA);
    ASSERT_EQ(dec.handle, TRACE_START_OBJ);
    ASSERT_EQ(dec.command, MS1_CMD_OBJECT2);
    ASSERT_EQ(dec.node, TRACE_MASTER_NODE);

    /* Response contains SD-Handle = 0x0008 at Data[4..5] */
    uint8_t resp_data[8] = {0x40, 0x2B, 0x00, 0x00, 0x00, 0x08, 0x00, 0x01};
    ASSERT_EQ(resp_data[0], MS1_DATA_SD_REQUEST);
    ASSERT_EQ(ms1_data_get_u16(resp_data, 4), TRACE_SD_HANDLE);

    TEST_PASS();
}

/* ========================================================================
 * Test 9: Trace - Name on the SD-Handle
 *
 * From page 38 (section 7.5, master renames the slave to "KS MS1"; the
 * slave sends its name the same way, page 53):
 *     110 | SD-Handle | 001 | Master | DLC=8 | 02 00 00 00 4B 53 20 4D
 *     110 | SD-Handle | 001 | Master | DLC=7 | 02 00 00 04 53 31 00
 * Data: {group 2, item 0, sub 0, offset, up to 4 characters}; the
 * terminating 0 ends the name.
 * ====================================================================== */

static void test_trace_lok_name(void)
{
    TEST_BEGIN("trace_lok_name");

    /* Name exchange is on SD-Handle (0x0008) with STRING command */
    uint32_t name_id = trace_can_id(6, TRACE_SD_HANDLE, 1, TRACE_MASTER_NODE);

    ms1_op_id_t dec;
    ms1_id_decode_op(name_id, &dec);
    ASSERT_EQ(dec.priority, MS1_PRIO_DATA);
    ASSERT_EQ(dec.handle, TRACE_SD_HANDLE);
    ASSERT_EQ(dec.command, MS1_CMD_STRING);
    ASSERT_EQ(dec.node, TRACE_MASTER_NODE);

    /* Name segment at offset 0: Data[0]=0x02(NAME), Data[3]=0x00(offset) */
    uint8_t name_seg0[8] = {0x02, 0x00, 0x00, 0x00, 0x4B, 0x53, 0x20, 0x4D};
    ASSERT_EQ(name_seg0[0], MS1_DATA_NAME);
    ASSERT_EQ(name_seg0[3], 0x00);  /* offset */
    /* ASCII at Data[4..7]: 'K'=0x4B, 'S'=0x53, ' '=0x20, 'M'=0x4D */
    ASSERT_EQ(name_seg0[4], 'K');
    ASSERT_EQ(name_seg0[5], 'S');
    ASSERT_EQ(name_seg0[6], ' ');
    ASSERT_EQ(name_seg0[7], 'M');

    /* Name segment at offset 4: Data = {02, 00, 00, 04, 53, 31, 00} = "S1\0" */
    uint8_t name_seg4[7] = {0x02, 0x00, 0x00, 0x04, 0x53, 0x31, 0x00};
    ASSERT_EQ(name_seg4[0], MS1_DATA_NAME);
    ASSERT_EQ(name_seg4[3], 0x04);  /* offset */
    ASSERT_EQ(name_seg4[4], 'S');
    ASSERT_EQ(name_seg4[5], '1');
    ASSERT_EQ(name_seg4[6], 0x00);

    TEST_PASS();
}

/* ========================================================================
 * Test 10: Trace - Lokstack size query on the SD-Handle
 *
 * From page 53 (setup phase): the ECoS asks the slave's object, the slave
 * answers with 10 places:
 *     110 | 00000000 00001000 | 001 | 0101010 | 4 | 41 00 00 00
 *     110 | 00000000 00001000 | 001 | 0101011 | 6 | 41 00 00 00 00 0A
 * ====================================================================== */

static void test_trace_lokstack_size(void)
{
    TEST_BEGIN("trace_lokstack_size_query");

    /* Lokstack size query on SD-Handle from the master */
    uint32_t query_id = trace_can_id(6, TRACE_SD_HANDLE, 1, TRACE_MASTER_NODE);

    ms1_op_id_t dec;
    ms1_id_decode_op(query_id, &dec);
    ASSERT_EQ(dec.priority, MS1_PRIO_DATA);
    ASSERT_EQ(dec.handle, TRACE_SD_HANDLE);
    ASSERT_EQ(dec.command, MS1_CMD_STRING);
    ASSERT_EQ(dec.node, TRACE_MASTER_NODE);

    /* Query data: marker = 0x41 (LOKSTACK_SZ); answer data[5] = 10 places */
    uint8_t query_data[1] = {MS1_DATA_LOKSTACK_SZ};
    ASSERT_EQ(query_data[0], 0x41);
    uint8_t answer[6] = {0x41, 0x00, 0x00, 0x00, 0x00, 0x0A};
    ASSERT_EQ(answer[5], MS1_LOKSTACK_SIZE);

    TEST_PASS();
}

/* ========================================================================
 * Test 11: Protocol constants verification
 * ====================================================================== */

static void test_protocol_constants(void)
{
    TEST_BEGIN("protocol_constants");

    /* Node numbers */
    ASSERT_EQ(MS1_NODE_REG, 126);
    ASSERT_EQ(MS1_NODE_FLASH, 127);

    /* Priorities (0 = highest). Speed, function and status values go at
     * priority 2 in both directions: the slave drives with 0808012B, the
     * master answers and drives with 0808012A (reference capture). */
    ASSERT_EQ(MS1_PRIO_DIRECT_LOK, 0);
    ASSERT_EQ(MS1_PRIO_STATE, 2);
    ASSERT_EQ(MS1_PRIO_SLAVE_DRIVE, MS1_PRIO_STATE);
    ASSERT_EQ(MS1_PRIO_MASTER_DRIVE, MS1_PRIO_STATE);
    ASSERT_EQ(MS1_PRIO_HEARTBEAT, 3);
    ASSERT_EQ(MS1_PRIO_STOP_GO, 4);
    ASSERT_EQ(MS1_PRIO_ASSIGNMENT, 5);
    ASSERT_EQ(MS1_PRIO_DATA, 6);
    ASSERT_EQ(MS1_PRIO_REG_FLASH, 7);

    /* Command values */
    ASSERT_EQ(MS1_CMD_STATUS, 0);
    ASSERT_EQ(MS1_CMD_STRING, 1);
    ASSERT_EQ(MS1_CMD_LIST, 2);
    ASSERT_EQ(MS1_CMD_OBJECT, 3);
    ASSERT_EQ(MS1_CMD_OBJECT2, 4);
    ASSERT_EQ(MS1_CMD_DELETE, 5);
    ASSERT_EQ(MS1_CMD_MONITOR, 7);

    /* Data markers */
    ASSERT_EQ(MS1_DATA_SD_REQUEST, 0x40);
    ASSERT_EQ(MS1_DATA_LOKSTACK_SZ, 0x41);
    ASSERT_EQ(MS1_DATA_NAME, 0x02);
    ASSERT_EQ(MS1_DATA_SY_REQUEST, 0x03);
    ASSERT_EQ(MS1_DATA_LOKSTACK, 0x80);

    /* Master types from trace context */
    ASSERT_EQ(MS1_MASTER_TYPE_ECOS, 0x22);
    ASSERT_EQ(MS1_MASTER_TYPE_CS1, 0x86);
    ASSERT_EQ(MS1_MASTER_TYPE_MS1, 0x84);
    ASSERT_EQ(MS1_VENDOR_ESU, 0x97);

    /* Baud rate */
    ASSERT_EQ(MS1_BAUD_RATE, 250000);

    TEST_PASS();
}

/* ========================================================================
 * Test 12: CAN ID bit field isolation
 *
 * Verify that each field occupies its correct bit positions by encoding
 * single fields and checking the resulting CAN ID.
 * ====================================================================== */

static void test_op_id_bit_positions(void)
{
    TEST_BEGIN("op_id_bit_positions");

    ms1_op_id_t oid = {0, 0, 0, 0};

    /* Priority only (bits 28..26) */
    oid.priority = 1; oid.handle = 0; oid.command = 0; oid.node = 0;
    ASSERT_EQ(ms1_id_encode_op(&oid), 1u << 26);

    /* Handle only (bits 25..10) */
    oid.priority = 0; oid.handle = 1; oid.command = 0; oid.node = 0;
    ASSERT_EQ(ms1_id_encode_op(&oid), 1u << 10);

    /* Command only (bits 9..7) */
    oid.priority = 0; oid.handle = 0; oid.command = 1; oid.node = 0;
    ASSERT_EQ(ms1_id_encode_op(&oid), 1u << 7);

    /* Node only (bits 6..0) */
    oid.priority = 0; oid.handle = 0; oid.command = 0; oid.node = 1;
    ASSERT_EQ(ms1_id_encode_op(&oid), 1u);

    TEST_PASS();
}

static void test_reg_id_bit_positions(void)
{
    TEST_BEGIN("reg_id_bit_positions");

    ms1_reg_id_t rid = {0, 0, 0, 0, 0, 0};

    /* Registration uses operational layout: prio(28-26) | handle(25-10) | direction(9-7) | node(6-0)
     * Handle sub-fields: uid_byte(15-8) | stage(7-5) | MID(4-0).
     * The MS1 answers a slave with command field 1 (1C0000FE to 1C00007E) and
     * the ECoS's stage-0 answer carries MID 12 in handle bits 4-0 (trace,
     * page 53: 00000000 00001100 | 001 | 1111110). */

    /* Priority only (CAN ID bits 28..26) */
    rid.priority = 1;
    ASSERT_EQ(ms1_id_encode_reg(&rid), 1u << 26);

    /* uid_byte=1 → handle bit 8 → CAN ID bit 18 */
    rid.priority = 0; rid.uid_byte = 1;
    ASSERT_EQ(ms1_id_encode_reg(&rid), 1u << 18);

    /* stage=1 → handle bit 5 → CAN ID bit 15 */
    rid.uid_byte = 0; rid.stage = 1;
    ASSERT_EQ(ms1_id_encode_reg(&rid), 1u << 15);

    /* MID=1 → handle bit 0 → CAN ID bit 10 */
    rid.stage = 0; rid.mid = 1;
    ASSERT_EQ(ms1_id_encode_reg(&rid), 1u << 10);

    /* direction=1 → command field → CAN ID bit 7 */
    rid.mid = 0; rid.direction = 1;
    ASSERT_EQ(ms1_id_encode_reg(&rid), 1u << 7);

    /* Node only (CAN ID bits 6..0) */
    rid.direction = 0; rid.node = 1;
    ASSERT_EQ(ms1_id_encode_reg(&rid), 1u);

    /* The ECoS's stage-0 answer from the trace */
    ms1_reg_id_t dec;
    ms1_id_decode_reg((7u << 26) | (0x000Cu << 10) | (1u << 7) | 126u, &dec);
    ASSERT_EQ(dec.stage, 0);
    ASSERT_EQ(dec.mid, 12);
    ASSERT_EQ(dec.direction, MS1_DIR_FROM_MASTER);

    TEST_PASS();
}

/* ========================================================================
 * Test 13: Trace - Assignment frame on lokstack
 *
 * From page 56:
 *   101 | 00000000 0001XXXX | 000 | 0101010 | 6 | 03 00 00 08 00 01
 *   Priority=5 (ASSIGNMENT), cmd=0 (STATUS), node=42 (master)
 *   Data[0]=0x03 (ASSIGNMENT marker), Data[3]=0x08, Data[5]=0x01 (grant)
 * ====================================================================== */

static void test_trace_assignment(void)
{
    TEST_BEGIN("trace_assignment_frame");

    /* Assignment uses PRIO_ASSIGNMENT and CMD_STATUS */
    ms1_op_id_t oid;
    oid.priority = MS1_PRIO_ASSIGNMENT;
    oid.handle = 0x0001;
    oid.command = MS1_CMD_STATUS;
    oid.node = TRACE_MASTER_NODE;

    uint32_t can_id = ms1_id_encode_op(&oid);
    ms1_op_id_t dec;
    ms1_id_decode_op(can_id, &dec);

    ASSERT_EQ(dec.priority, MS1_PRIO_ASSIGNMENT);
    ASSERT_EQ(dec.command, MS1_CMD_STATUS);
    ASSERT_EQ(dec.node, TRACE_MASTER_NODE);

    /* Assignment grant data */
    uint8_t grant_data[6] = {MS1_DATA_ASSIGNMENT, 0x00, 0x00, 0x08, 0x00, 0x01};
    ASSERT_EQ(grant_data[0], 0x03);

    TEST_PASS();
}

/* ========================================================================
 * Mock CAN bus for master integration tests
 * ====================================================================== */

#define MOCK_TX_MAX 64
#define MOCK_RX_MAX 32

static ms1_can_frame_t mock_tx_buf[MOCK_TX_MAX];
static int mock_tx_count = 0;

static ms1_can_frame_t mock_rx_buf[MOCK_RX_MAX];
static int mock_rx_head = 0;
static int mock_rx_tail = 0;

static uint32_t mock_time_ms = 0;

static bool mock_can_send(const ms1_can_frame_t *frame)
{
    if (mock_tx_count >= MOCK_TX_MAX) return false;
    mock_tx_buf[mock_tx_count++] = *frame;
    return true;
}

static bool mock_can_receive(ms1_can_frame_t *frame)
{
    if (mock_rx_head == mock_rx_tail) return false;
    *frame = mock_rx_buf[mock_rx_head++];
    return true;
}

static uint32_t mock_millis(void)
{
    return mock_time_ms;
}

static void mock_reset(void)
{
    mock_tx_count = 0;
    mock_rx_head = 0;
    mock_rx_tail = 0;
    mock_time_ms = 0;
}

static void mock_inject_frame(uint32_t id, uint8_t dlc, const uint8_t *data)
{
    if (mock_rx_head == mock_rx_tail) {
        mock_rx_head = mock_rx_tail = 0;
    }
    if (mock_rx_tail >= MOCK_RX_MAX) return;
    mock_rx_buf[mock_rx_tail].id = id;
    mock_rx_buf[mock_rx_tail].dlc = dlc;
    if (data && dlc > 0) {
        memcpy(mock_rx_buf[mock_rx_tail].data, data, dlc);
    } else {
        memset(mock_rx_buf[mock_rx_tail].data, 0, 8);
    }
    mock_rx_tail++;
}

static ms1_platform_t mock_platform = {
    .can_send = mock_can_send,
    .can_receive = mock_can_receive,
    .millis = mock_millis,
    .log = NULL,
};

/* ========================================================================
 * Test 14: Master initialization
 * ====================================================================== */

static void test_master_init(void)
{
    TEST_BEGIN("master_init");

    mock_reset();

    ms1_master_t master;
    ms1_master_config_t config = {
        .uid = {0x00, 0x04, 0x84, 0xD3},
        .hw_version = 0x4010,
        .sw_version = 0x0301,
        .build_version = 0x0001,
        .vendor_id = MS1_VENDOR_ESU,
        .master_type = MS1_MASTER_TYPE_ECOS,
        .node_base = 42,
    };

    ms1_master_init(&master, &mock_platform, &config, NULL);

    /* Verify initial state */
    ASSERT_EQ(master.stopped, false);
    ASSERT_EQ(master.overcurrent, false);
    ASSERT_EQ(master.next_mid, 0);
    ASSERT_EQ(master.next_flash_handle, 0x02);
    for (int i = 0; i < MS1_MAX_LOKS; i++) {
        ASSERT_EQ(master.lok_controller[i], -1);
    }

    /* All slave slots should be idle */
    for (int i = 0; i < MS1_MAX_SLAVES; i++) {
        ms1_slave_info_t info;
        ASSERT_TRUE(ms1_master_get_slave_info(&master, i, &info));
        ASSERT_EQ(info.state, MS1_SLAVE_IDLE);
        ASSERT_EQ(info.selected_lok, -1);
    }

    /* No loks active */
    for (int i = 0; i < MS1_MAX_LOKS; i++) {
        ASSERT_EQ(master.loks[i].active, false);
    }

    TEST_PASS();
}

/* ========================================================================
 * Test 15: Master sends bus-occupied on first poll
 * ====================================================================== */

static void test_master_bus_occupied(void)
{
    TEST_BEGIN("master_bus_occupied_burst");

    mock_reset();

    ms1_master_t master;
    ms1_master_config_t config = {
        .uid = {0x01, 0x02, 0x03, 0x04},
        .hw_version = 0x4010,
        .sw_version = 0x0301,
        .build_version = 0x0001,
        .vendor_id = MS1_VENDOR_ESU,
        .master_type = MS1_MASTER_TYPE_ECOS,
        .node_base = 42,
    };

    ms1_master_init(&master, &mock_platform, &config, NULL);

    /* First poll should send bus-occupied frames */
    ms1_master_poll(&master);

    /* Should have sent at least one bus-occupied frame */
    ASSERT_TRUE(mock_tx_count >= 1);

    /* Verify bus-occupied frame */
    ASSERT_EQ(mock_tx_buf[0].id, MS1_ID_BUS_OCCUPIED);
    ASSERT_EQ(mock_tx_buf[0].dlc, 8);

    /* All data bytes should be zero */
    for (int i = 0; i < 8; i++) {
        ASSERT_EQ(mock_tx_buf[0].data[i], 0);
    }

    /* Eight at start-up, as the MS1 master sends them, and never again: to a
     * slave whose master node is 0 they are a reset request */
    ASSERT_EQ(mock_tx_count, MS1_BUS_OCCUPIED_BURST_COUNT);
    for (int i = 0; i < mock_tx_count; i++) {
        ASSERT_EQ(mock_tx_buf[i].id, MS1_ID_BUS_OCCUPIED);
    }
    mock_tx_count = 0;
    mock_time_ms = 6000;
    ms1_master_poll(&master);
    ASSERT_EQ(mock_tx_count, 0);

    TEST_PASS();
}

/* ========================================================================
 * Test 16: Master handles registration stage 0
 * ====================================================================== */

static void test_master_registration_stage0(void)
{
    TEST_BEGIN("master_registration_stage0");

    mock_reset();

    ms1_master_t master;
    ms1_master_config_t config = {
        .uid = {0x00, 0x04, 0x84, 0xD3},
        .hw_version = 0x4010,
        .sw_version = 0x0301,
        .build_version = 0x0001,
        .vendor_id = MS1_VENDOR_ESU,
        .master_type = MS1_MASTER_TYPE_ECOS,
        .node_base = 42,
    };

    ms1_master_init(&master, &mock_platform, &config, NULL);
    master.bus_occupied_burst_remaining = 0; /* Skip bus-occupied */

    /* Inject a registration stage 0 frame from slave (as trace row 3, whose
     * UID byte is 0) with the UID byte 0x12 */
    ms1_reg_id_t slave_reg;
    slave_reg.priority = MS1_PRIO_REG_FLASH;
    slave_reg.uid_byte = 0;  /* UID byte 3 = 0 */
    slave_reg.stage = MS1_REG_STAGE_UID3;
    slave_reg.direction = MS1_DIR_FROM_SLAVE;
    slave_reg.node = MS1_NODE_REG;
    slave_reg.mid = 0;

    uint32_t reg_id = ms1_id_encode_reg(&slave_reg);
    ASSERT_EQ(reg_id, 0x1C00007E);  /* Matches trace */
    slave_reg.uid_byte = 0x12;
    mock_inject_frame(ms1_id_encode_reg(&slave_reg), 0, NULL);

    /* Poll to process the frame */
    mock_tx_count = 0;
    ms1_master_poll(&master);

    /* Master should have sent a response */
    ASSERT_TRUE(mock_tx_count >= 1);

    /* Find the response frame (on node 126, direction=master) */
    ms1_can_frame_t *resp = NULL;
    for (int i = 0; i < mock_tx_count; i++) {
        if ((mock_tx_buf[i].id & 0x7F) == MS1_NODE_REG) {
            resp = &mock_tx_buf[i];
            break;
        }
    }
    ASSERT_TRUE(resp != NULL);

    /* Decode the response */
    ms1_reg_id_t resp_reg;
    ms1_id_decode_reg(resp->id, &resp_reg);
    ASSERT_EQ(resp_reg.priority, MS1_PRIO_REG_FLASH);
    ASSERT_EQ(resp_reg.direction, MS1_DIR_FROM_MASTER);
    ASSERT_EQ(resp_reg.node, MS1_NODE_REG);
    ASSERT_EQ(resp_reg.stage, MS1_REG_STAGE_UID3);
    /* The slave's UID byte echoed (an MS1 slave restarts its registration
     * when the answer carries another), with the master's MID */
    ASSERT_EQ(resp_reg.uid_byte, 0x12);
    ASSERT_EQ(resp_reg.mid, 0);

    /* Response data: master UID, then the monitoring handle (0) as the MS1
     * master sends it: 00 02 00 00 */
    ASSERT_EQ(resp->dlc, 8);
    ASSERT_EQ(resp->data[0], config.uid[0]);
    ASSERT_EQ(resp->data[1], config.uid[1]);
    ASSERT_EQ(resp->data[2], config.uid[2]);
    ASSERT_EQ(resp->data[3], config.uid[3]);
    ASSERT_EQ(resp->data[5], 0x02);
    ASSERT_EQ(ms1_data_get_u16(resp->data, 6), MS1_HANDLE_MONITOR);

    /* Slave should now be in REG_STAGE_1 */
    ms1_slave_info_t info;
    ms1_master_get_slave_info(&master, 0, &info);
    ASSERT_EQ(info.state, MS1_SLAVE_REG_STAGE_1);

    TEST_PASS();
}

/* ========================================================================
 * Test 17: Master lok database
 * ====================================================================== */

static void test_master_lok_database(void)
{
    TEST_BEGIN("master_lok_database");

    mock_reset();

    ms1_master_t master;
    ms1_master_config_t config = {
        .uid = {0x01, 0x02, 0x03, 0x04},
        .hw_version = 0x4010,
        .sw_version = 0x0301,
        .build_version = 0x0001,
        .vendor_id = MS1_VENDOR_ESU,
        .master_type = MS1_MASTER_TYPE_ECOS,
        .node_base = 42,
    };

    ms1_master_init(&master, &mock_platform, &config, NULL);

    /* Create a lok (like "003 131-0" from the trace) */
    ms1_lok_t lok;
    memset(&lok, 0, sizeof(lok));
    strncpy(lok.name, "003 131-0", MS1_LOK_NAME_LEN);
    lok.type = MS1_LOK_TYPE_STEAM;
    lok.format = MS1_RAIL_MFX;
    lok.func_count = 3;
    lok.funcs[0].number = 0x01;  /* Speed */
    lok.funcs[0].activation = MS1_FUNC_SPEED;
    lok.funcs[0].icon = MS1_ICON_NONE;
    lok.funcs[1].number = 0x02;  /* Light */
    lok.funcs[1].activation = MS1_FUNC_MAINTAINED;
    lok.funcs[1].icon = MS1_ICON_01;
    lok.funcs[2].number = 0x03;  /* Sound */
    lok.funcs[2].activation = MS1_FUNC_MAINTAINED;
    lok.funcs[2].icon = MS1_ICON_02;

    ASSERT_TRUE(ms1_master_set_lok(&master, 0, &lok));
    ASSERT_TRUE(master.loks[0].active);
    ASSERT_EQ(master.loks[0].type, MS1_LOK_TYPE_STEAM);
    ASSERT_EQ(master.loks[0].format, MS1_RAIL_MFX);
    ASSERT_EQ(master.loks[0].func_count, 3);
    ASSERT_TRUE(strcmp(master.loks[0].name, "003 131-0") == 0);

    /* Out of range slot */
    ASSERT_TRUE(!ms1_master_set_lok(&master, MS1_MAX_LOKS, &lok));

    /* Remove lok */
    ASSERT_TRUE(ms1_master_remove_lok(&master, 0));
    ASSERT_TRUE(!master.loks[0].active);

    TEST_PASS();
}

/* ========================================================================
 * Test 18: Verify CAN ID matches between encode helpers and trace_can_id
 *
 * Ensures the trace table format → CAN ID → field decode → re-encode
 * is fully consistent.
 * ====================================================================== */

static void test_trace_id_consistency(void)
{
    TEST_BEGIN("trace_id_consistency");

    /* For each frame type from the trace, verify:
     * trace_can_id(p,h,c,n) == ms1_id_encode_op(p,h,c,n) */

    struct { uint8_t p; uint16_t h; uint8_t c; uint8_t n; uint32_t expected; } cases[] = {
        /* Bus occupied */
        { 0, 0x0000, 7, 0, 0x00000380 },
        /* Heartbeat slave */
        { 3, 0x0001, 7, 43, 0x0C0007AB },
        /* Heartbeat master */
        { 3, 0x0001, 7, 42, 0x0C0007AA },
        /* SD-handle request */
        { 6, 0x0002, 4, 43, 0x18000A2B },
        /* SD-handle response */
        { 6, 0x0002, 4, 42, 0x18000A2A },
    };

    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        uint32_t from_trace = trace_can_id(cases[i].p, cases[i].h, cases[i].c, cases[i].n);

        ms1_op_id_t oid;
        oid.priority = cases[i].p;
        oid.handle = cases[i].h;
        oid.command = cases[i].c;
        oid.node = cases[i].n;
        uint32_t from_encode = ms1_id_encode_op(&oid);

        ASSERT_EQ(from_trace, cases[i].expected);
        ASSERT_EQ(from_encode, cases[i].expected);
    }

    TEST_PASS();
}

/* ========================================================================
 * Test 19: System status (stop/go/overcurrent)
 * ====================================================================== */

static void test_master_stop_go(void)
{
    TEST_BEGIN("master_stop_go");

    mock_reset();

    ms1_master_t master;
    ms1_master_config_t config = {
        .uid = {0x01, 0x02, 0x03, 0x04},
        .hw_version = 0x4010,
        .sw_version = 0x0301,
        .build_version = 0x0001,
        .vendor_id = MS1_VENDOR_ESU,
        .master_type = MS1_MASTER_TYPE_ECOS,
        .node_base = 42,
    };

    ms1_master_init(&master, &mock_platform, &config, NULL);

    ASSERT_EQ(master.stopped, false);
    ASSERT_EQ(master.overcurrent, false);

    ms1_master_set_stop(&master, true);
    ASSERT_EQ(master.stopped, true);

    ms1_master_set_stop(&master, false);
    ASSERT_EQ(master.stopped, false);

    ms1_master_set_overcurrent(&master, true);
    ASSERT_EQ(master.overcurrent, true);

    ms1_master_set_overcurrent(&master, false);
    ASSERT_EQ(master.overcurrent, false);

    TEST_PASS();
}

/* ========================================================================
 * Master integration: a slave registers, sets up, drives
 *
 * The slave's frames are those an MS1 slave sends (ms1_reference.can, and the
 * ECoS trace for node 43); node_base 42 as an ECoS.
 * ====================================================================== */

static int cb_connected_n, cb_disconnected_n, cb_speed_n, cb_function_n, cb_stop_n, cb_go_n;
static int cb_selected_last = -2;
static uint8_t cb_speed_last, cb_function_last[2];

static void tcb_connected(void *ctx, uint8_t s) { (void)ctx; (void)s; cb_connected_n++; }
static void tcb_disconnected(void *ctx, uint8_t s) { (void)ctx; (void)s; cb_disconnected_n++; }
static void tcb_speed(void *ctx, uint8_t s, uint8_t l, uint8_t v) { (void)ctx; (void)s; (void)l; cb_speed_n++; cb_speed_last = v; }
static void tcb_function(void *ctx, uint8_t s, uint8_t l, uint8_t f, uint8_t v)
{
    (void)ctx; (void)s; (void)l;
    cb_function_n++;
    cb_function_last[0] = f;
    cb_function_last[1] = v;
}
static void tcb_stop(void *ctx, uint8_t s) { (void)ctx; (void)s; cb_stop_n++; }
static void tcb_go(void *ctx, uint8_t s) { (void)ctx; (void)s; cb_go_n++; }
static void tcb_selected(void *ctx, uint8_t s, int8_t l) { (void)ctx; (void)s; cb_selected_last = l; }

static ms1_master_t im;   /* integration master */

static void integration_init(void)
{
    mock_reset();
    cb_connected_n = cb_disconnected_n = cb_speed_n = cb_function_n = cb_stop_n = cb_go_n = 0;
    cb_selected_last = -2;
    ms1_master_config_t config = {
        .uid = {0x00, 0x04, 0x84, 0xD3}, .hw_version = 0x4010, .sw_version = 0x0301,
        .vendor_id = MS1_VENDOR_ESU, .master_type = MS1_MASTER_TYPE_ECOS, .node_base = 42,
    };
    ms1_master_callbacks_t cb = {0};
    cb.on_slave_connected = tcb_connected;
    cb.on_slave_disconnected = tcb_disconnected;
    cb.on_lok_speed = tcb_speed;
    cb.on_lok_function = tcb_function;
    cb.on_stop = tcb_stop;
    cb.on_go = tcb_go;
    cb.on_lok_selected = tcb_selected;
    ms1_master_init(&im, &mock_platform, &config, &cb);
    ms1_master_poll(&im); /* bus-occupied frames */

    ms1_lok_t lok;
    memset(&lok, 0, sizeof lok);
    strcpy(lok.name, "003 131-0");
    lok.type = MS1_LOK_TYPE_STEAM;
    lok.format = MS1_RAIL_MFX;
    lok.func_count = 3;
    lok.funcs[0] = (ms1_lok_func_t){.number = 1, .activation = MS1_FUNC_SPEED};
    lok.funcs[1] = (ms1_lok_func_t){.number = 2, .activation = MS1_FUNC_MAINTAINED, .icon = MS1_ICON_01};
    lok.funcs[2] = (ms1_lok_func_t){.number = 3, .activation = MS1_FUNC_MOMENTARY, .icon = MS1_ICON_02};
    ms1_master_set_lok(&im, 0, &lok);
    mock_tx_count = 0;
}

/* A slave frame on its connection (slave node 43) */
static void slave_op(uint8_t prio, uint16_t handle, uint8_t cmd, uint8_t dlc, const uint8_t *data)
{
    mock_inject_frame(trace_can_id(prio, handle, cmd, TRACE_SLAVE_NODE), dlc, data);
    mock_tx_count = 0;
    ms1_master_poll(&im);
}

/* The master's frame sent at index i is ID, DLC, data */
static bool tx_is(int i, uint32_t id, uint8_t dlc, const uint8_t *data)
{
    return i < mock_tx_count && mock_tx_buf[i].id == id && mock_tx_buf[i].dlc == dlc &&
           memcmp(mock_tx_buf[i].data, data, dlc) == 0;
}

#define M_ID(prio, handle, cmd) trace_can_id(prio, handle, cmd, TRACE_MASTER_NODE)

/* Registration stages 0-3 of the trace's slave (UID 00 02 04 B3), which is to
 * get the given slave node; returns false at the first wrong answer */
static bool register_slave(uint8_t slave_node)
{
    ms1_reg_id_t r = {MS1_PRIO_REG_FLASH, 0, 0, MS1_DIR_FROM_SLAVE, MS1_NODE_REG, 0};
    uint8_t mid = 0;
    for (uint8_t k = 0; k < 4; k++) {
        r.uid_byte = TRACE_SLAVE_UID[k];
        r.stage = k;
        r.mid = k ? mid : 0;
        mock_inject_frame(ms1_id_encode_reg(&r), 0, NULL);
        mock_tx_count = 0;
        ms1_master_poll(&im);
        if (mock_tx_count != (k == 3 ? 2 : 1)) return false;
        ms1_reg_id_t a;
        ms1_id_decode_reg(mock_tx_buf[0].id, &a);
        if (a.uid_byte != r.uid_byte || a.stage != k || a.direction != MS1_DIR_FROM_MASTER ||
            a.node != MS1_NODE_REG || (k && a.mid != mid)) return false;
        mid = a.mid;
    }
    /* Stage 7 right after stage 3: the slave's UID, start object, node 43 with
     * bit 7 (master node = 42), data[7] 0 (bit 0 would start the bootloader).
     * The ECoS sends 00 02 04 B3 00 02 AB 00 (start object 2). */
    ms1_reg_id_t s7 = {MS1_PRIO_REG_FLASH, 0, 7, MS1_DIR_FROM_MASTER, MS1_NODE_REG, mid};
    uint8_t d7[8] = {0x00, 0x02, 0x04, 0xB3, 0x00, 0x01, (uint8_t)(0x80 | slave_node), 0x00};
    return tx_is(1, ms1_id_encode_reg(&s7), 8, d7);
}

/* Setup as an MS1 slave does it (reference capture, 9.36-9.40 s), the
 * lokstack holding lok 0 when assign is set */
static bool setup_slave(bool assign)
{
    uint8_t own[2] = {0x40, 0x2B};
    slave_op(6, MS1_HANDLE_START, MS1_CMD_OBJECT2, 2, own);
    uint8_t own_r[8] = {0x40, 0x2B, 0x00, 0x00, 0x01, 0x2B, 0x00, 0x01};
    if (!tx_is(0, M_ID(6, MS1_HANDLE_START, MS1_CMD_OBJECT2), 8, own_r)) return false;

    uint8_t sy[2] = {0x03, 0x00};
    slave_op(6, MS1_HANDLE_START, MS1_CMD_OBJECT, 2, sy);
    uint8_t sy_r[8] = {0x03, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01};
    if (!tx_is(0, M_ID(6, MS1_HANDLE_START, MS1_CMD_OBJECT), 8, sy_r)) return false;

    for (uint8_t i = 1; i <= 5; i++) {
        uint8_t q[2] = {i, 0x01};
        slave_op(6, MS1_HANDLE_STATUS, MS1_CMD_LIST, 2, q);
        uint8_t kind[3] = {i, 0x01, i};
        uint8_t value[3] = {i, 0x00, 0x00};
        if (i <= 4 && !(tx_is(0, M_ID(6, MS1_HANDLE_STATUS, MS1_CMD_LIST), 3, kind) &&
                        tx_is(1, M_ID(2, MS1_HANDLE_STATUS, MS1_CMD_LIST), 3, value))) return false;
        if (i == 5 && !tx_is(0, M_ID(6, MS1_HANDLE_STATUS, MS1_CMD_LIST), 2, kind)) return false;
    }

    if (assign && !ms1_master_assign_lok(&im, 0, 0)) return false;
    uint8_t q80[4] = {0x80, 0x02, 0x00, 0x00};
    slave_op(6, MS1_HANDLE_START, MS1_CMD_OBJECT, 4, q80);
    if (assign) {
        uint8_t r80[8] = {0x80, 0x02, 0x00, 0x00, 0x02, 0x00, 0x00, 0x01};
        if (!tx_is(0, M_ID(6, MS1_HANDLE_START, MS1_CMD_OBJECT), 8, r80)) return false;
        uint8_t q80b[4] = {0x80, 0x02, 0x02, 0x00};
        slave_op(6, MS1_HANDLE_START, MS1_CMD_OBJECT, 4, q80b);
        if (!tx_is(0, M_ID(6, MS1_HANDLE_START, MS1_CMD_OBJECT), 4, q80b)) return false;
    } else if (!tx_is(0, M_ID(6, MS1_HANDLE_START, MS1_CMD_OBJECT), 4, q80)) {
        return false;
    }
    uint8_t q81[4] = {0x81, 0x02, 0x00, 0x00};
    slave_op(6, MS1_HANDLE_START, MS1_CMD_OBJECT, 4, q81);
    if (cb_connected_n != 0) return false;
    uint8_t q82[4] = {0x82, 0x02, 0x00, 0x00};
    slave_op(6, MS1_HANDLE_START, MS1_CMD_OBJECT, 4, q82);
    return tx_is(0, M_ID(6, MS1_HANDLE_START, MS1_CMD_OBJECT), 4, q82) && cb_connected_n == 1;
}

static void test_master_registration_setup(void)
{
    TEST_BEGIN("master_registration_setup");
    integration_init();
    ASSERT_TRUE(register_slave(TRACE_SLAVE_NODE));

    ms1_slave_info_t info;
    ms1_master_get_slave_info(&im, 0, &info);
    ASSERT_EQ(info.state, MS1_SLAVE_SETUP);
    ASSERT_MEM_EQ(info.uid, TRACE_SLAVE_UID, 4);
    ASSERT_EQ(info.master_node, TRACE_MASTER_NODE);
    ASSERT_EQ(info.slave_node, TRACE_SLAVE_NODE);

    /* The slave's stage 4-7 frames need no answer */
    ms1_reg_id_t r = {MS1_PRIO_REG_FLASH, 0, 4, MS1_DIR_FROM_SLAVE, MS1_NODE_REG, 0};
    uint8_t d4[8] = {0x00, 0x02, 0x04, 0xB3, 0x00, 0x02, 0x00, 0x03};
    mock_inject_frame(ms1_id_encode_reg(&r), 8, d4);
    mock_tx_count = 0;
    ms1_master_poll(&im);
    ASSERT_EQ(mock_tx_count, 0);

    ASSERT_TRUE(setup_slave(false));
    ms1_master_get_slave_info(&im, 0, &info);
    ASSERT_EQ(info.state, MS1_SLAVE_OPERATIONAL);
    TEST_PASS();
}

static void test_master_registration_mid(void)
{
    TEST_BEGIN("master_registration_mid");
    integration_init();

    /* Two slaves start together (same first UID byte): one answer, one MID;
     * the second stage-0 frame is ignored while the registration runs (an
     * MS1 slave restarts on any answer that is not its own). Their stage-1
     * frames differ; the first one through wins, the other slave sees
     * another UID byte echoed and restarts. */
    ms1_reg_id_t r = {MS1_PRIO_REG_FLASH, 0x00, 0, MS1_DIR_FROM_SLAVE, MS1_NODE_REG, 0};
    mock_inject_frame(ms1_id_encode_reg(&r), 0, NULL);
    mock_inject_frame(ms1_id_encode_reg(&r), 0, NULL);
    mock_tx_count = 0;
    ms1_master_poll(&im);
    ASSERT_EQ(mock_tx_count, 1);
    ms1_reg_id_t a;
    ms1_id_decode_reg(mock_tx_buf[0].id, &a);
    uint8_t mid = a.mid;

    r.stage = 1; r.mid = mid; r.uid_byte = 0x02;
    mock_inject_frame(ms1_id_encode_reg(&r), 0, NULL);
    r.uid_byte = 0x05;
    mock_inject_frame(ms1_id_encode_reg(&r), 0, NULL);
    mock_tx_count = 0;
    ms1_master_poll(&im);
    ASSERT_EQ(mock_tx_count, 1);
    ms1_id_decode_reg(mock_tx_buf[0].id, &a);
    ASSERT_EQ(a.uid_byte, 0x02);

    /* A frame with another MID belongs to no registration */
    r.stage = 2; r.mid = (uint8_t)((mid + 1) & 0x1F); r.uid_byte = 0x04;
    mock_inject_frame(ms1_id_encode_reg(&r), 0, NULL);
    mock_tx_count = 0;
    ms1_master_poll(&im);
    ASSERT_EQ(mock_tx_count, 0);

    /* A registration that stops is dropped after MS1_REG_TIMEOUT_MS */
    mock_time_ms += MS1_REG_TIMEOUT_MS;
    ms1_master_poll(&im);
    ms1_slave_info_t info;
    ms1_master_get_slave_info(&im, 0, &info);
    ASSERT_EQ(info.state, MS1_SLAVE_IDLE);
    TEST_PASS();
}

static void test_master_lok_setup(void)
{
    TEST_BEGIN("master_lok_setup");
    integration_init();
    ASSERT_TRUE(register_slave(TRACE_SLAVE_NODE));
    ASSERT_TRUE(setup_slave(true));

    /* The slave's requests for lok 0x0200, answered on its handle */
    uint16_t h = MS1_HANDLE_LOK;
    struct { uint8_t cmd, qdlc, q[4], prio, rdlc, r[8]; } x[] = {
        {1, 4, {0x40, 0x03, 0x00, 0x00}, 6, 6, {0x40, 0x03, 0x00, 0x00, 0x00, 0x03}},           /* symbol */
        {1, 4, {0x40, 0x01, 0x00, 0x00}, 6, 8, {0x40, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00}}, /* mfx */
        {1, 4, {0x40, 0x01, 0x01, 0x00}, 6, 8, {0x40, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}}, /* address */
        {2, 2, {0x01, 0x01}, 6, 4, {0x01, 0x01, 0x01, 0x01}},                                 /* speed object */
        {2, 2, {0x01, 0x00}, 2, 3, {0x01, 0x00, 0x00}},
        {2, 2, {0x02, 0x01}, 6, 4, {0x02, 0x01, 0x02, 0x01}},                                 /* function, icon 1 */
        {2, 2, {0x03, 0x01}, 6, 4, {0x03, 0x01, 0x02, 0x82}},                                 /* momentary: bit 7 */
        {2, 2, {0x04, 0x01}, 6, 2, {0x04, 0x01}},                                             /* end of list */
        {2, 4, {0x01, 0x02, 0x01, 0x00}, 6, 5, {0x01, 0x02, 0x01, 0x00, 0x04}},               /* acceleration */
        {1, 4, {0x40, 0x04, 0x00, 0x00}, 6, 6, {0x40, 0x04, 0x00, 0x00, 0x00, 0xFF}},         /* volume */
        {0, 2, {0x03, 0x00}, 5, 7, {0x03, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00}},               /* controller: none */
        {1, 4, {0x02, 0x00, 0x00, 0x00}, 6, 8, {0x02, 0x00, 0x00, 0x00, '0', '0', '3', ' '}},
        {1, 4, {0x02, 0x00, 0x00, 0x04}, 6, 8, {0x02, 0x00, 0x00, 0x04, '1', '3', '1', '-'}},
        {1, 4, {0x02, 0x00, 0x00, 0x08}, 6, 6, {0x02, 0x00, 0x00, 0x08, '0', 0x00}},
    };
    for (size_t i = 0; i < sizeof x / sizeof x[0]; i++) {
        slave_op(6, h, x[i].cmd, x[i].qdlc, x[i].q);
        ASSERT_EQ(mock_tx_count, 1);
        ASSERT_TRUE(tx_is(0, M_ID(x[i].prio, h, x[i].cmd), x[i].rdlc, x[i].r));
    }
    TEST_PASS();
}

static void test_master_take_drive_release(void)
{
    TEST_BEGIN("master_take_drive_release");
    integration_init();
    ASSERT_TRUE(register_slave(TRACE_SLAVE_NODE));
    ASSERT_TRUE(setup_slave(true));
    uint16_t h = MS1_HANDLE_LOK;

    /* Driving before taking the lok: no callback, the current value back */
    uint8_t speed[3] = {0x01, 0x00, 0x23};
    slave_op(2, h, MS1_CMD_LIST, 3, speed);
    uint8_t zero[3] = {0x01, 0x00, 0x00};
    ASSERT_TRUE(tx_is(0, M_ID(2, h, MS1_CMD_LIST), 3, zero));
    ASSERT_EQ(cb_speed_n, 0);

    /* Take: {3, 0, own object, counter}; the master names the slave as controller */
    uint8_t take[6] = {0x03, 0x00, 0x01, 0x2B, 0x00, 0x01};
    slave_op(5, h, MS1_CMD_STATUS, 6, take);
    uint8_t taken[7] = {0x03, 0x00, 0x01, 0x2B, 0x00, 0x01, 0x00};
    ASSERT_TRUE(tx_is(0, M_ID(5, h, MS1_CMD_STATUS), 7, taken));
    ASSERT_EQ(cb_selected_last, 0);

    /* Speed, direction and functions at priority 2; each echoed */
    slave_op(2, h, MS1_CMD_LIST, 3, speed);
    ASSERT_TRUE(tx_is(0, M_ID(2, h, MS1_CMD_LIST), 3, speed));
    ASSERT_EQ(cb_speed_n, 1);
    ASSERT_EQ(cb_speed_last, 0x23);
    uint8_t fn[3] = {0x02, 0x00, 0x01};
    slave_op(2, h, MS1_CMD_LIST, 3, fn);
    ASSERT_TRUE(tx_is(0, M_ID(2, h, MS1_CMD_LIST), 3, fn));
    ASSERT_EQ(cb_function_last[0], 2);
    ASSERT_EQ(cb_function_last[1], 1);

    /* STOP and GO from the slave (status object 1) */
    uint8_t stop[3] = {0x01, 0x00, 0x01};
    slave_op(2, MS1_HANDLE_STATUS, MS1_CMD_LIST, 3, stop);
    ASSERT_EQ(cb_stop_n, 1);
    ASSERT_TRUE(im.stopped);
    uint8_t go[3] = {0x01, 0x00, 0x00};
    slave_op(2, MS1_HANDLE_STATUS, MS1_CMD_LIST, 3, go);
    ASSERT_EQ(cb_go_n, 1);
    ASSERT_TRUE(!im.stopped);

    /* The master's STOP and driving reach the slave at priority 2 */
    mock_tx_count = 0;
    ms1_master_set_stop(&im, true);
    ASSERT_TRUE(tx_is(0, M_ID(2, MS1_HANDLE_STATUS, MS1_CMD_LIST), 3, stop));
    mock_tx_count = 0;
    ms1_master_drive_lok(&im, 0, 0x85);
    uint8_t rev[3] = {0x01, 0x00, 0x85};
    ASSERT_TRUE(tx_is(0, M_ID(2, h, MS1_CMD_LIST), 3, rev));

    /* Release: {3, 1, own object, counter}; controller 0 */
    uint8_t release[6] = {0x03, 0x01, 0x01, 0x2B, 0x00, 0x01};
    slave_op(5, h, MS1_CMD_STATUS, 6, release);
    uint8_t freed[7] = {0x03, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
    ASSERT_TRUE(tx_is(0, M_ID(5, h, MS1_CMD_STATUS), 7, freed));
    ASSERT_EQ(cb_selected_last, -1);

    /* Unassign: {0, 0, lok, counter} on the start object (command 5) */
    mock_tx_count = 0;
    ASSERT_TRUE(ms1_master_unassign_lok(&im, 0, 0));
    uint8_t gone[8] = {0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x01};
    ASSERT_TRUE(tx_is(0, M_ID(6, MS1_HANDLE_START, MS1_CMD_DELETE), 8, gone));
    TEST_PASS();
}

static void test_master_monitoring(void)
{
    TEST_BEGIN("master_monitoring");
    integration_init();
    ASSERT_TRUE(register_slave(TRACE_SLAVE_NODE));
    ASSERT_TRUE(setup_slave(false));

    /* The slave's poll {0, master node}: answered with the master's UID (as
     * the ECoS trace and the MS1 master; an MS1 slave restarts when words
     * 2-3 are neither the master's nor its own UID) */
    uint8_t poll[4] = {0x00, 0x00, 0x00, 0x2A};
    slave_op(3, MS1_HANDLE_MONITOR, MS1_CMD_MONITOR, 4, poll);
    uint8_t ans[8] = {0x00, 0x00, 0x00, 0x2A, 0x00, 0x04, 0x84, 0xD3};
    ASSERT_TRUE(tx_is(0, M_ID(3, MS1_HANDLE_MONITOR, MS1_CMD_MONITOR), 8, ans));

    /* Silent for MS1_HEARTBEAT_INTERVAL_MS: the master polls {0, slave node,
     * slave UID}; the slave's answer (the same data) gets no reply */
    uint32_t t0 = mock_time_ms;
    mock_tx_count = 0;
    mock_time_ms = t0 + MS1_HEARTBEAT_INTERVAL_MS - 1;
    ms1_master_poll(&im);
    ASSERT_EQ(mock_tx_count, 0);
    mock_time_ms = t0 + MS1_HEARTBEAT_INTERVAL_MS;
    ms1_master_poll(&im);
    uint8_t mpoll[8] = {0x00, 0x00, 0x00, 0x2B, 0x00, 0x02, 0x04, 0xB3};
    ASSERT_TRUE(tx_is(0, M_ID(3, MS1_HANDLE_MONITOR, MS1_CMD_MONITOR), 8, mpoll));
    mock_inject_frame(trace_can_id(3, MS1_HANDLE_MONITOR, MS1_CMD_MONITOR, TRACE_SLAVE_NODE), 8, mpoll);
    mock_tx_count = 0;
    ms1_master_poll(&im);
    ASSERT_EQ(mock_tx_count, 0);

    /* Then silence: a poll every interval, given up after MS1_HEARTBEAT_TIMEOUT_MS */
    uint32_t last = mock_time_ms;
    int polls = 0;
    for (uint32_t t = last; t < last + MS1_HEARTBEAT_TIMEOUT_MS - 1; t++) {
        mock_time_ms = t;
        mock_tx_count = 0;
        ms1_master_poll(&im);
        polls += mock_tx_count;
    }
    ASSERT_EQ(polls, (MS1_HEARTBEAT_TIMEOUT_MS - 2) / MS1_HEARTBEAT_INTERVAL_MS);
    ASSERT_EQ(cb_disconnected_n, 0);
    mock_time_ms = last + MS1_HEARTBEAT_TIMEOUT_MS;
    ms1_master_poll(&im);
    ASSERT_EQ(cb_disconnected_n, 1);
    ms1_slave_info_t info;
    ms1_master_get_slave_info(&im, 0, &info);
    ASSERT_EQ(info.state, MS1_SLAVE_IDLE);
    TEST_PASS();
}

static void test_master_rename_and_reregister(void)
{
    TEST_BEGIN("master_rename_and_reregister");
    integration_init();
    ASSERT_TRUE(register_slave(TRACE_SLAVE_NODE));
    ASSERT_TRUE(setup_slave(true));

    /* Rename: the frames of the document (page 38) on the slave's own object */
    mock_tx_count = 0;
    ms1_master_rename_slave(&im, 0, "KS MS1");
    uint8_t n0[8] = {0x02, 0x00, 0x00, 0x00, 0x4B, 0x53, 0x20, 0x4D};
    uint8_t n4[7] = {0x02, 0x00, 0x00, 0x04, 0x53, 0x31, 0x00};
    ASSERT_EQ(mock_tx_count, 2);
    ASSERT_TRUE(tx_is(0, M_ID(6, 0x012B, MS1_CMD_STRING), 8, n0));
    ASSERT_TRUE(tx_is(1, M_ID(6, 0x012B, MS1_CMD_STRING), 7, n4));

    /* The slave takes the lok, restarts and registers again: the old
     * connection is dropped (callback), the lok is free again */
    uint8_t take[6] = {0x03, 0x00, 0x01, 0x2B, 0x00, 0x01};
    slave_op(5, MS1_HANDLE_LOK, MS1_CMD_STATUS, 6, take);
    ASSERT_EQ(im.lok_controller[0], 0);
    ASSERT_TRUE(register_slave(45)); /* in slave context 1: nodes 44/45 */
    ASSERT_EQ(cb_disconnected_n, 1);
    ASSERT_EQ(im.lok_controller[0], -1);
    ms1_slave_info_t info;
    ms1_master_get_slave_info(&im, 0, &info);
    ASSERT_EQ(info.state, MS1_SLAVE_IDLE);
    ms1_master_get_slave_info(&im, 1, &info);
    ASSERT_EQ(info.state, MS1_SLAVE_SETUP);
    ASSERT_EQ(info.master_node, 44);
    ASSERT_EQ(info.slave_node, 45);
    TEST_PASS();
}

/* ========================================================================
 * Main
 * ====================================================================== */

int main(void)
{
    printf("M-CAN Protocol Tests (ms1_test_trace)\n");
    printf("Trace data from CAN_Doku_V101_de.pdf, Chapter 11 (pages 53-56)\n");
    printf("ECoS FW 3.0.1 + MS1 slave, Lok 003 131-0\n");
    printf("======================================================\n\n");

    /* CAN ID encoding/decoding */
    test_bus_occupied_id();
    test_op_id_roundtrip();
    test_reg_id_roundtrip();
    test_flash_id_roundtrip();
    test_op_id_bit_positions();
    test_reg_id_bit_positions();
    test_data_helpers();
    test_protocol_constants();

    /* Trace data verification */
    test_trace_reg_stage0();
    test_trace_heartbeat();
    test_trace_sd_handle();
    test_trace_lok_name();
    test_trace_lokstack_size();
    test_trace_assignment();
    test_trace_id_consistency();

    /* Master integration tests */
    test_master_init();
    test_master_bus_occupied();
    test_master_registration_stage0();
    test_master_lok_database();
    test_master_stop_go();
    test_master_registration_setup();
    test_master_registration_mid();
    test_master_lok_setup();
    test_master_take_drive_release();
    test_master_monitoring();
    test_master_rename_and_reregister();

    printf("\n======================================================\n");
    printf("Results: %d/%d tests passed\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
