/**
 * @file ms1_types.h
 * @brief M-CAN protocol constants, types, and CAN ID encoding/decoding.
 *
 * This header defines the M-CAN protocol (Märklin CAN bus protocol used
 * between ECoS/CS1/MS1-Master and MS1 slaves). It provides:
 *   - CAN ID bit field definitions for registration, operational, and flash phases
 *   - Protocol constants (priorities, commands, status types, etc.)
 *   - CAN ID encode/decode functions
 *   - Data byte helpers
 */

#ifndef MS1_TYPES_H
#define MS1_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Compile-time configuration (override before including this header)
 * ====================================================================== */

#ifndef MS1_MAX_SLAVES
#define MS1_MAX_SLAVES      4
#endif

#ifndef MS1_MAX_LOKS
#define MS1_MAX_LOKS        20
#endif

#ifndef MS1_MAX_LOK_FUNCS
#define MS1_MAX_LOK_FUNCS   16
#endif

#ifndef MS1_LOKSTACK_SIZE
#define MS1_LOKSTACK_SIZE   10
#endif

#ifndef MS1_LOK_NAME_LEN
#define MS1_LOK_NAME_LEN    16
#endif

#ifndef MS1_SLAVE_NAME_LEN
#define MS1_SLAVE_NAME_LEN  16
#endif

/* ========================================================================
 * CAN bus parameters
 * ====================================================================== */

#define MS1_BAUD_RATE       250000  /**< 250 kBaud */

/* ========================================================================
 * Special node numbers
 * ====================================================================== */

#define MS1_NODE_REG        126     /**< Node number during registration */
#define MS1_NODE_FLASH      127     /**< Node number during flashing */

/* ========================================================================
 * Special CAN IDs
 * ====================================================================== */

#define MS1_ID_BUS_OCCUPIED 0x00000380u  /**< Bus-occupied probe frame */

/* ========================================================================
 * Registration frame CAN ID bit layout (29 bits)
 *
 * Registration frames use the operational layout (below) with node 126; the
 * 16-bit handle carries the registration fields and the command field the
 * direction:
 *
 *   CAN ID level:
 *     Bit 28..26 : Priority (3)    = 0b111
 *     Bit 25..10 : Handle (16)     = UID byte | stage | MID (below)
 *     Bit 9..7   : Command (3)     = direction: 0 = from slave, 1 = from master
 *     Bit 6..0   : Node (7)        = 126
 *
 *   Handle sub-fields:
 *     Handle bit 15..8  : UID byte (8) — the slave's UID byte of stages 0-3 (MSB
 *                         first); 0 in stages 4-7. The master echoes the slave's byte.
 *     Handle bit  7..5  : Stage (3)    — 0..7
 *     Handle bit  4..0  : MID (5)      — registration number: the master picks it in
 *                         its stage-0 answer, both sides repeat it in every later frame
 *
 * The MS1 firmware (mcan_process) checks all of these: a slave restarts its
 * registration when an answer echoes another UID byte or MID.
 * ====================================================================== */

/* CAN ID level shifts (same as operational) */
#define MS1_REG_PRIO_SHIFT     26
#define MS1_REG_PRIO_MASK      0x07u

#define MS1_REG_HANDLE_SHIFT   10
#define MS1_REG_HANDLE_MASK    0xFFFFu  /* 16 bits */

#define MS1_REG_CMD_SHIFT      7
#define MS1_REG_CMD_MASK       0x07u

#define MS1_REG_NODE_SHIFT     0
#define MS1_REG_NODE_MASK      0x7Fu

/* Handle sub-field shifts (within the 16-bit handle) */
#define MS1_REG_UID_SHIFT      8
#define MS1_REG_UID_MASK       0xFFu   /* 8 bits */

#define MS1_REG_STAGE_SHIFT    5
#define MS1_REG_STAGE_MASK     0x07u   /* 3 bits */

#define MS1_REG_MID_SHIFT      0
#define MS1_REG_MID_MASK       0x1Fu   /* 5 bits */

/* Direction: the command field (CAN ID bits 9..7), 0 or 1 */
#define MS1_REG_DIR_SHIFT      MS1_REG_CMD_SHIFT
#define MS1_REG_DIR_MASK       MS1_REG_CMD_MASK

/* ========================================================================
 * Operational frame CAN ID bit layout (29 bits)
 *
 *   Bit 28..26 : Priority (3)
 *   Bit 25..10 : Object-Handle (16)
 *   Bit 9..7   : Command (3)
 *   Bit 6..0   : Node number (7)
 * ====================================================================== */

#define MS1_OP_PRIO_SHIFT      26
#define MS1_OP_PRIO_MASK       0x07u

#define MS1_OP_HANDLE_SHIFT    10
#define MS1_OP_HANDLE_MASK     0xFFFFu  /* 16 bits */

#define MS1_OP_CMD_SHIFT       7
#define MS1_OP_CMD_MASK        0x07u

#define MS1_OP_NODE_SHIFT      0
#define MS1_OP_NODE_MASK       0x7Fu

/* ========================================================================
 * Flash frame CAN ID bit layout (29 bits)
 *
 *   Bit 28..26 : Priority (3)    = 0b111
 *   Bit 25..18 : Flash-Handle (8)
 *   Bit 17..15 : Request (3)
 *   Bit 14..10 : Stufe/stage (5)
 *   Bit 9..7   : Flags (3)
 *   Bit 6..0   : Node (7)        = 127
 * ====================================================================== */

#define MS1_FLASH_PRIO_SHIFT    26
#define MS1_FLASH_HANDLE_SHIFT  18
#define MS1_FLASH_HANDLE_MASK   0xFFu
#define MS1_FLASH_REQ_SHIFT     15
#define MS1_FLASH_REQ_MASK      0x07u
#define MS1_FLASH_STAGE_SHIFT   10
#define MS1_FLASH_STAGE_MASK    0x1Fu
#define MS1_FLASH_FLAGS_SHIFT   7
#define MS1_FLASH_FLAGS_MASK    0x07u
#define MS1_FLASH_NODE_SHIFT    0
#define MS1_FLASH_NODE_MASK     0x7Fu

/* ========================================================================
 * Priority values (3 bits, 0=highest, 7=lowest)
 *
 * Priorities only order frames on the bus: the MS1 firmware dispatches a
 * received frame by its command field and handle and ignores the priority.
 * The values the MS1 sends:
 * ====================================================================== */

typedef enum {
    MS1_PRIO_DIRECT_LOK   = 0,  /**< Highest; not used by the MS1 */
    MS1_PRIO_STATE        = 2,  /**< Speed, function and system-status values, both directions */
    MS1_PRIO_SLAVE_DRIVE  = MS1_PRIO_STATE, /**< Slave drives a lok */
    MS1_PRIO_MASTER_DRIVE = MS1_PRIO_STATE, /**< Master drives a lok */
    MS1_PRIO_HEARTBEAT    = 3,  /**< Cyclic monitoring */
    MS1_PRIO_STOP_GO      = 4,  /**< System status as an ECoS sends it; the MS1 uses MS1_PRIO_STATE */
    MS1_PRIO_ASSIGNMENT   = 5,  /**< Controller of a lok (take / release / query) */
    MS1_PRIO_DATA         = 6,  /**< Requests and their answers (setup, lok data) */
    MS1_PRIO_REG_FLASH    = 7,  /**< Registration & flash (lowest) */
} ms1_priority_t;

/* ========================================================================
 * Command values (3 bits, operational frames)
 * ====================================================================== */

typedef enum {
    MS1_CMD_STATUS     = 0,  /**< Controller of a lok: {3, 0 take | 1 release, controller, counter} */
    MS1_CMD_STRING     = 1,  /**< Object data: {group, item, sub, offset, value...} (lok data, names) */
    MS1_CMD_LIST       = 2,  /**< Values and kinds: {object, 0, value} / {object, 1[, kind]} */
    MS1_CMD_OBJECT     = 3,  /**< Next object of a kind (status object, lokstack iteration) */
    MS1_CMD_OBJECT2    = 4,  /**< Object announcement, the slave's own object */
    MS1_CMD_DELETE     = 5,  /**< Object removed */
    MS1_CMD_UNUSED     = 6,  /**< Unused in M-CAN (CS2 protocol) */
    MS1_CMD_MONITOR    = 7,  /**< Cyclic monitoring + bus-occupied */
} ms1_command_t;

/* ========================================================================
 * Object handles the master hands out (operational frames)
 *
 * The slave learns each handle from the master and only needs them to be
 * distinct; these are the MS1 master's values. A handle of the slave's own
 * object or the status object in a remove frame restarts an MS1 slave.
 * ====================================================================== */

#define MS1_HANDLE_MONITOR   0x0000u  /**< Cyclic monitoring (stage-0 answer data[6..7]) */
#define MS1_HANDLE_START     0x0001u  /**< Start object: setup requests, lok announcements (stage-7 answer) */
#define MS1_HANDLE_STATUS    0x0002u  /**< System status object (SY handle) */
#define MS1_HANDLE_DEVICE    0x0100u  /**< Device objects: slave's own object = this + slave node (SD handle) */
#define MS1_HANDLE_LOK       0x0200u  /**< Lok objects: this + slot in the master's lok database */

/* ========================================================================
 * Registration stages (0..7)
 * ====================================================================== */

typedef enum {
    MS1_REG_STAGE_UID3 = 0,  /**< UID byte 3 (MSB) */
    MS1_REG_STAGE_UID2 = 1,  /**< UID byte 2 */
    MS1_REG_STAGE_UID1 = 2,  /**< UID byte 1 */
    MS1_REG_STAGE_UID0 = 3,  /**< UID byte 0 (LSB) */
    MS1_REG_STAGE_HW   = 4,  /**< Hardware/power info */
    MS1_REG_STAGE_SW   = 5,  /**< Software version */
    MS1_REG_STAGE_BL   = 6,  /**< Build/bootloader info */
    MS1_REG_STAGE_NODE = 7,  /**< Node number assignment */
} ms1_reg_stage_t;

/* ========================================================================
 * Direction bit values
 * ====================================================================== */

#define MS1_DIR_FROM_SLAVE   0
#define MS1_DIR_FROM_MASTER  1

/* ========================================================================
 * Locomotive types
 * ====================================================================== */

typedef enum {
    MS1_LOK_TYPE_NONE    = 0x00,
    MS1_LOK_TYPE_ELECTRIC = 0x01,
    MS1_LOK_TYPE_DIESEL  = 0x02,
    MS1_LOK_TYPE_STEAM   = 0x03,
} ms1_lok_type_t;

/* ========================================================================
 * Rail format / track protocol
 * ====================================================================== */

typedef enum {
    MS1_RAIL_MFX       = 0x01,
    MS1_RAIL_DCC       = 0x02,
    MS1_RAIL_MM        = 0x03,  /**< Motorola / Selectrix */
} ms1_rail_format_t;

/* ========================================================================
 * Function activation types
 * ====================================================================== */

/* On the wire (answer to a kind query {object, 1}): the speed object is kind
 * 0x0101 ({object, 1, 0x01, 0x01}), a function kind 0x02 with its icon
 * ({object, 1, 0x02, icon}), bit 7 of the icon set for a momentary one. */
typedef enum {
    MS1_FUNC_SPEED      = 0x00,  /**< Speed and direction (the speed object) */
    MS1_FUNC_MAINTAINED = 0x01,  /**< Dauerfunktion (toggle on/off) */
    MS1_FUNC_MOMENTARY  = 0x02,  /**< Momentanbetrieb (active while pressed) */
} ms1_func_activation_t;

/* ========================================================================
 * Function icons (displayed on MS1 LCD)
 *
 * The MS1 groups functions by icon 0x01-0x10 and puts each on a key; it keeps
 * at most 9 functions and drops one with icon 0 (MS1_ICON_NONE).
 * ====================================================================== */

typedef enum {
    MS1_ICON_NONE  = 0x00,
    MS1_ICON_01    = 0x01,
    MS1_ICON_02    = 0x02,
    MS1_ICON_03    = 0x03,
    MS1_ICON_04    = 0x04,
    MS1_ICON_05    = 0x05,
    MS1_ICON_06    = 0x06,
    MS1_ICON_07    = 0x07,
    MS1_ICON_08    = 0x08,
    MS1_ICON_09    = 0x09,
    MS1_ICON_0A    = 0x0A,
    MS1_ICON_0B    = 0x0B,  /**< Default / no icon displayed */
    MS1_ICON_0C    = 0x0C,
    MS1_ICON_0D    = 0x0D,
    MS1_ICON_0E    = 0x0E,
    MS1_ICON_0F    = 0x0F,
    MS1_ICON_10    = 0x10,  /**< Generic function (the MS1 master's own default) */
} ms1_icon_t;

/* ========================================================================
 * System status types
 *
 * The master offers status objects 1-4 with these kinds (as the MS1 master
 * does); the slave asks each object's kind during setup and then exchanges
 * {object, 0, value} frames on MS1_HANDLE_STATUS.
 * ====================================================================== */

typedef enum {
    MS1_STATUS_STOP_GO     = 0x01,  /**< 1=Stop, 0=Go (from a slave, bit 1 = emergency stop) */
    MS1_STATUS_OVERCURRENT = 0x02,  /**< 1=Kurzschluss, 0=Normal */
    MS1_STATUS_MS1_ONLY    = 0x03,  /**< MS1 master only: 1 while it runs an MM programming job */
    MS1_STATUS_MFX_REG     = 0x04,  /**< 1 = mfx registration running (blinking mfx icon) */
} ms1_status_type_t;

/* ========================================================================
 * Master type identification bytes
 * ====================================================================== */

#define MS1_MASTER_TYPE_MS1   0x84
#define MS1_MASTER_TYPE_ECOS  0x22
#define MS1_MASTER_TYPE_CS1   0x86

/* ========================================================================
 * Data byte magic values used in the protocol
 * ====================================================================== */

#define MS1_DATA_SD_REQUEST   0x40  /**< Own-object request (SD handle); group of lok data */
#define MS1_DATA_LOKSTACK_SZ  0x41  /**< Lokstack size (asked of the slave by an ECoS) */
#define MS1_DATA_NAME         0x02  /**< Name group */
#define MS1_DATA_SY_REQUEST   0x03  /**< Status-object request (SY handle) */
#define MS1_DATA_LOKSTACK     0x80  /**< Object kind of a lok (lokstack iteration, announcement) */
#define MS1_DATA_ASSIGNMENT   0x03  /**< Controller frames */

/* Name/string segment size */
#define MS1_NAME_SEGMENT_LEN  4

/* ========================================================================
 * NMRA vendor ID
 * ====================================================================== */

#define MS1_VENDOR_ESU        0x97  /**< ESU NMRA manufacturer code */

/* ========================================================================
 * Timing constants (milliseconds)
 * ====================================================================== */

/* The MS1 runs its CAN protocol in 4 ms ticks: it polls a silent partner after
 * 64 ticks and gives it up after 0x4FF ticks (mcan_process). */
#define MS1_HEARTBEAT_INTERVAL_MS    256   /**< Poll a slave silent this long (and again each period) */
#define MS1_HEARTBEAT_TIMEOUT_MS     5116  /**< Slave silent this long = gone; also the slave's own limit */
#define MS1_REG_TIMEOUT_MS           5116  /**< Registration without progress this long is dropped */
#define MS1_BUS_OCCUPIED_BURST_COUNT 8     /**< 0x380 frames sent once at start-up */

/* ========================================================================
 * Decoded registration frame fields
 * ====================================================================== */

typedef struct {
    uint8_t  priority;   /**< 3 bits (CAN ID bits 28-26) */
    uint8_t  uid_byte;   /**< 8 bits (handle bits 15-8): UID byte for stages 0-3, 0 for 4-7 */
    uint8_t  stage;      /**< 3 bits (handle bits 7-5): registration stage 0-7 */
    uint8_t  direction;  /**< command field (CAN ID bits 9-7): 0=slave, 1=master */
    uint8_t  node;       /**< 7 bits (CAN ID bits 6-0) */
    uint8_t  mid;        /**< 5 bits (handle bits 4-0): registration number (MID) */
} ms1_reg_id_t;

/* ========================================================================
 * Decoded operational frame fields
 * ====================================================================== */

typedef struct {
    uint8_t  priority;  /**< 3 bits */
    uint16_t handle;    /**< 16 bits object handle */
    uint8_t  command;   /**< 3 bits */
    uint8_t  node;      /**< 7 bits */
} ms1_op_id_t;

/* ========================================================================
 * Decoded flash frame fields
 * ====================================================================== */

typedef struct {
    uint8_t  priority;      /**< 3 bits */
    uint8_t  flash_handle;  /**< 8 bits */
    uint8_t  request;       /**< 3 bits */
    uint8_t  stage;         /**< 5 bits */
    uint8_t  flags;         /**< 3 bits */
    uint8_t  node;          /**< 7 bits */
} ms1_flash_id_t;

/* ========================================================================
 * CAN ID encode/decode functions
 * ====================================================================== */

/** Encode a registration-phase CAN ID. */
uint32_t ms1_id_encode_reg(const ms1_reg_id_t *id);

/** Decode a 29-bit CAN ID into registration-phase fields. */
void ms1_id_decode_reg(uint32_t can_id, ms1_reg_id_t *id);

/** Encode an operational-phase CAN ID. */
uint32_t ms1_id_encode_op(const ms1_op_id_t *id);

/** Decode a 29-bit CAN ID into operational-phase fields. */
void ms1_id_decode_op(uint32_t can_id, ms1_op_id_t *id);

/** Encode a flash-phase CAN ID. */
uint32_t ms1_id_encode_flash(const ms1_flash_id_t *id);

/** Decode a 29-bit CAN ID into flash-phase fields. */
void ms1_id_decode_flash(uint32_t can_id, ms1_flash_id_t *id);

/* ========================================================================
 * Data byte helpers (big-endian / Motorola byte order)
 * ====================================================================== */

/** Write a 16-bit value into data[offset] and data[offset+1] (big-endian). */
static inline void ms1_data_put_u16(uint8_t *data, uint8_t offset, uint16_t val)
{
    data[offset]     = (uint8_t)(val >> 8);
    data[offset + 1] = (uint8_t)(val & 0xFF);
}

/** Read a 16-bit value from data[offset] and data[offset+1] (big-endian). */
static inline uint16_t ms1_data_get_u16(const uint8_t *data, uint8_t offset)
{
    return (uint16_t)((uint16_t)data[offset] << 8) | data[offset + 1];
}

/** Write a 32-bit value into data[offset..offset+3] (big-endian). */
static inline void ms1_data_put_u32(uint8_t *data, uint8_t offset, uint32_t val)
{
    data[offset]     = (uint8_t)(val >> 24);
    data[offset + 1] = (uint8_t)(val >> 16);
    data[offset + 2] = (uint8_t)(val >> 8);
    data[offset + 3] = (uint8_t)(val & 0xFF);
}

/** Read a 32-bit value from data[offset..offset+3] (big-endian). */
static inline uint32_t ms1_data_get_u32(const uint8_t *data, uint8_t offset)
{
    return ((uint32_t)data[offset] << 24)
         | ((uint32_t)data[offset + 1] << 16)
         | ((uint32_t)data[offset + 2] << 8)
         |  (uint32_t)data[offset + 3];
}

#ifdef __cplusplus
}
#endif

#endif /* MS1_TYPES_H */
