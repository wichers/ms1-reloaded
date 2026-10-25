/**
 * @file cs2_types.h
 * @brief CS2 CAN protocol constants, types, and CAN ID encoding/decoding.
 *
 * This header defines the Märklin CS2 CAN protocol v2.0 used between
 * CS2/CS3 central stations, MS2 throttles, Gleisboxes, and other
 * compatible devices. It provides:
 *   - CAN ID bit field definitions (priority, command, response, hash)
 *   - Protocol command and sub-command constants
 *   - Device types, Loc-ID address ranges
 *   - CAN ID encode/decode functions
 *   - Hash calculation from UID
 *   - CRC-16/CCITT for config data streaming
 *   - Loc-ID encode/decode helpers
 */

#ifndef CS2_TYPES_H
#define CS2_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Compile-time configuration (override before including this header)
 * ====================================================================== */

#ifndef CS2_MAX_DEVICES
#define CS2_MAX_DEVICES      8
#endif

#ifndef CS2_MAX_LOKS
#define CS2_MAX_LOKS         40
#endif

#ifndef CS2_MAX_ACCESSORIES
#define CS2_MAX_ACCESSORIES  64
#endif

#ifndef CS2_LOK_NAME_LEN
#define CS2_LOK_NAME_LEN     16
#endif

#ifndef CS2_MAX_LOK_FUNCS
#define CS2_MAX_LOK_FUNCS    32
#endif

#ifndef CS2_CONFIG_BUF_SIZE
#define CS2_CONFIG_BUF_SIZE  4096
#endif

/* ========================================================================
 * CAN bus parameters
 * ====================================================================== */

#define CS2_BAUD_RATE  250000  /**< 250 kBaud */

/* ========================================================================
 * CAN ID bit layout (29 bits)
 *
 *   Bits 28..25 : Priority (4 bits)
 *   Bits 24..17 : Command  (8 bits)  — logical command value
 *   Bit  16     : Response (1 bit)   — 0=request, 1=response
 *   Bits 15..0  : Hash     (16 bits) — collision detection
 *
 * Encoding: can_id = (prio << 25) | (cmd << 17) | (resp << 16) | hash
 * ====================================================================== */

#define CS2_ID_PRIO_SHIFT   25
#define CS2_ID_PRIO_MASK    0x0Fu   /* 4 bits */

#define CS2_ID_CMD_SHIFT    17
#define CS2_ID_CMD_MASK     0xFFu   /* 8 bits */

#define CS2_ID_RESP_SHIFT   16
#define CS2_ID_RESP_MASK    0x01u   /* 1 bit */

#define CS2_ID_HASH_SHIFT   0
#define CS2_ID_HASH_MASK    0xFFFFu /* 16 bits */

/* ========================================================================
 * Decoded CAN ID fields
 * ====================================================================== */

typedef struct {
    uint8_t  priority;  /**< 4 bits (0=highest) */
    uint8_t  command;   /**< 8 bits — logical command value (0x00-0x22) */
    uint8_t  response;  /**< 1 bit — 0=request, 1=response */
    uint16_t hash;      /**< 16 bits — collision detection hash */
} cs2_can_id_t;

/* ========================================================================
 * Command values (8 bits)
 *
 * The "Wert in CAN-ID" from the spec is (command << 1) because the
 * response bit at bit 16 shifts the command field. We store the logical
 * command value here.
 * ====================================================================== */

typedef enum {
    CS2_CMD_SYSTEM          = 0x00,  /**< System commands (sub-cmd in data[4]) */
    CS2_CMD_LOK_DISCOVERY   = 0x01,  /**< Lok discovery */
    CS2_CMD_MFX_BIND        = 0x02,  /**< MFX bind */
    CS2_CMD_MFX_VERIFY      = 0x03,  /**< MFX verify */
    CS2_CMD_LOK_SPEED       = 0x04,  /**< Lok speed */
    CS2_CMD_LOK_DIRECTION   = 0x05,  /**< Lok direction */
    CS2_CMD_LOK_FUNCTION    = 0x06,  /**< Lok function */
    CS2_CMD_READ_CONFIG     = 0x07,  /**< Read CV config */
    CS2_CMD_WRITE_CONFIG    = 0x08,  /**< Write CV config */
    CS2_CMD_ACC_SWITCH      = 0x0B,  /**< Accessory switch */
    CS2_CMD_ACC_CONFIG      = 0x0C,  /**< Accessory configuration */
    CS2_CMD_S88_POLLING     = 0x10,  /**< S88 polling (compat mode) */
    CS2_CMD_S88_EVENT       = 0x11,  /**< S88 event / feedback */
    CS2_CMD_SX1_EVENT       = 0x12,  /**< SX1 event */
    CS2_CMD_PING            = 0x18,  /**< Software version / participant ping */
    CS2_CMD_UPDATE_OFFER    = 0x19,  /**< Firmware update offer */
    CS2_CMD_READ_CFG_DATA   = 0x1A,  /**< Read config data */
    CS2_CMD_BOOTLOADER_CAN  = 0x1B,  /**< Bootloader CAN-bound */
    CS2_CMD_BOOTLOADER_RAIL = 0x1C,  /**< Bootloader rail-bound */
    CS2_CMD_STATUS_CONFIG   = 0x1D,  /**< Status data configuration */
    CS2_CMD_CFG_DATA_REQ    = 0x20,  /**< Config data request */
    CS2_CMD_CFG_DATA_STREAM = 0x21,  /**< Config data stream */
    CS2_CMD_CONNECT_6021    = 0x22,  /**< Connect 6021 data stream */
} cs2_command_t;

/* ========================================================================
 * System sub-commands (data byte 4 of system command frames)
 * ====================================================================== */

typedef enum {
    CS2_SYS_STOP              = 0x00,  /**< System stop — track power off */
    CS2_SYS_GO                = 0x01,  /**< System go — track power on */
    CS2_SYS_HALT              = 0x02,  /**< System halt — all loks speed 0 */
    CS2_SYS_LOK_EMERGENCY     = 0x03,  /**< Lok emergency stop */
    CS2_SYS_LOK_CYCLE_STOP    = 0x04,  /**< Remove lok from refresh cycle */
    CS2_SYS_LOK_DATA_PROTOCOL = 0x05,  /**< Set lok data protocol variant */
    CS2_SYS_ACC_SWITCH_TIME   = 0x06,  /**< Set accessory switch time */
    CS2_SYS_FAST_READ_MFX     = 0x07,  /**< Fast read for MFX SID */
    CS2_SYS_RAIL_PROTOCOL     = 0x08,  /**< Enable/disable rail protocols */
    CS2_SYS_MFX_REG_COUNT     = 0x09,  /**< MFX registration counter */
    CS2_SYS_OVERLOAD          = 0x0A,  /**< Overload notification */
    CS2_SYS_STATUS            = 0x0B,  /**< System status query */
    CS2_SYS_SYSTEM_ID         = 0x0C,  /**< System identification */
    CS2_SYS_MFX_SEEK          = 0x30,  /**< MFX seek */
    CS2_SYS_RESET             = 0x80,  /**< System reset */
} cs2_sys_subcmd_t;

/* ========================================================================
 * Priority values (4 bits)
 * ====================================================================== */

typedef enum {
    CS2_PRIO_STOP_GO      = 1,  /**< Stop / Go / Kurzschluss */
    CS2_PRIO_FEEDBACK     = 2,  /**< Rückmeldungen */
    CS2_PRIO_LOK_HALT     = 3,  /**< Lok anhalten */
    CS2_PRIO_LOK_ACC      = 4,  /**< Lok / Zubehörbefehle */
} cs2_priority_t;

/* ========================================================================
 * Device types (from ping response, bytes 6-7)
 * ====================================================================== */

typedef enum {
    CS2_DEV_GFP          = 0x0000,  /**< GFP 60213/60214, Booster 60173/60174 */
    CS2_DEV_GLEISBOX     = 0x0010,  /**< Gleisbox 60112/60113 */
    CS2_DEV_CONNECT_6021 = 0x0020,  /**< Connect 6021 Art-Nr. 60128 */
    CS2_DEV_MS2          = 0x0030,  /**< MS2 60653, Txxxxx */
    CS2_DEV_WIRELESS     = 0xFFE0,  /**< Wireless devices */
    CS2_DEV_CS2_GUI      = 0xFFFF,  /**< CS2-GUI (Master) */
} cs2_device_type_t;

/* ========================================================================
 * Rail protocol / track format
 * ====================================================================== */

typedef enum {
    CS2_PROTO_MM   = 0,  /**< Märklin Motorola */
    CS2_PROTO_MFX  = 1,  /**< mfx */
    CS2_PROTO_DCC  = 2,  /**< DCC */
    CS2_PROTO_SX1  = 3,  /**< Selectrix 1 */
} cs2_protocol_t;

/* ========================================================================
 * Loc-ID address ranges (lower 16 bits of 32-bit Loc-ID)
 * ====================================================================== */

#define CS2_LOCID_MM_BASE       0x0000u
#define CS2_LOCID_MM_END        0x03FFu
#define CS2_LOCID_SX1_BASE      0x0800u
#define CS2_LOCID_SX1_END       0x0BFFu
#define CS2_LOCID_MM_ACC_BASE   0x3000u
#define CS2_LOCID_MM_ACC_END    0x33FFu
#define CS2_LOCID_DCC_ACC_BASE  0x3800u
#define CS2_LOCID_DCC_ACC_END   0x3FFFu
#define CS2_LOCID_MFX_BASE      0x4000u
#define CS2_LOCID_MFX_END       0x7FFFu
#define CS2_LOCID_DCC_BASE      0xC000u
#define CS2_LOCID_DCC_END       0xFFFFu

/* ========================================================================
 * Direction values
 * ====================================================================== */

typedef enum {
    CS2_DIR_KEEP     = 0,  /**< Keep current direction */
    CS2_DIR_FORWARD  = 1,  /**< Forward */
    CS2_DIR_BACKWARD = 2,  /**< Backward */
    CS2_DIR_TOGGLE   = 3,  /**< Toggle direction */
} cs2_direction_t;

/* ========================================================================
 * Lok speed range
 * ====================================================================== */

#define CS2_SPEED_MIN  0      /**< Stopped */
#define CS2_SPEED_MAX  1000   /**< Maximum speed */

/* ========================================================================
 * Function range
 * ====================================================================== */

#define CS2_FUNC_MIN  0   /**< F0 */
#define CS2_FUNC_MAX  31  /**< F31 */

/* ========================================================================
 * Broadcast UID
 * ====================================================================== */

#define CS2_UID_BROADCAST  0x00000000u  /**< Target all devices */

/* ========================================================================
 * Rail protocol enable bits (for SYS_RAIL_PROTOCOL sub-command)
 * ====================================================================== */

#define CS2_RAIL_BIT_MM2  (1u << 0)  /**< MM2 enabled */
#define CS2_RAIL_BIT_MFX  (1u << 1)  /**< mfx enabled */
#define CS2_RAIL_BIT_DCC  (1u << 2)  /**< DCC enabled */

/* ========================================================================
 * Timing constants (milliseconds)
 * ====================================================================== */

#define CS2_PING_INTERVAL_MS          5000   /**< Periodic ping interval */
#define CS2_DEVICE_TIMEOUT_MS         15000  /**< No activity = device lost */
#define CS2_CONFIG_STREAM_TIMEOUT_MS  5000   /**< Config stream timeout */

/* ========================================================================
 * Hash constants
 * ====================================================================== */

/** Mask to clear bits 7-9 of hash */
#define CS2_HASH_CLEAR_MASK  0xFC7Fu
/** Value to set bits 8-9 (CS1 compatibility: bits 9,8,7 = 1,1,0 = value 6) */
#define CS2_HASH_SET_BITS    0x0300u

/* ========================================================================
 * CRC-16 constants
 * ====================================================================== */

#define CS2_CRC_INIT   0xFFFFu  /**< CRC-16/CCITT initial value */
#define CS2_CRC_POLY   0x1021u  /**< CRC-16/CCITT polynomial */

/* ========================================================================
 * CAN ID encode/decode functions
 * ====================================================================== */

/** Encode a CS2 CAN ID from decoded fields. */
uint32_t cs2_id_encode(const cs2_can_id_t *id);

/** Decode a 29-bit CAN ID into CS2 fields. */
void cs2_id_decode(uint32_t can_id, cs2_can_id_t *id);

/* ========================================================================
 * Hash calculation
 * ====================================================================== */

/**
 * Calculate the 16-bit hash from a device UID.
 *
 * Algorithm: XOR upper 16 bits with lower 16 bits of UID, then fix
 * bits 7-9 to value 6 (0b110) for CS1 protocol compatibility.
 */
uint16_t cs2_hash_from_uid(uint32_t uid);

/* ========================================================================
 * CRC-16/CCITT
 * ====================================================================== */

/**
 * Calculate CRC-16/CCITT over a data buffer.
 *
 * Polynomial: 0x1021, initial value: 0xFFFF.
 * Used for config data streaming integrity checks.
 */
uint16_t cs2_crc16(const uint8_t *data, uint32_t length);

/* ========================================================================
 * Loc-ID helpers
 * ====================================================================== */

/**
 * Encode a locomotive address and protocol into a 32-bit Loc-ID.
 *
 * @param address  Protocol-specific address (e.g., MM 1-255, DCC 1-9999)
 * @param protocol Track protocol (MM, MFX, DCC, SX1)
 * @return 32-bit Loc-ID with protocol base offset applied
 */
uint32_t cs2_locid_encode(uint16_t address, cs2_protocol_t protocol);

/**
 * Extract the protocol-specific address from a Loc-ID.
 */
uint16_t cs2_locid_address(uint32_t locid);

/**
 * Determine the track protocol from a Loc-ID.
 */
cs2_protocol_t cs2_locid_protocol(uint32_t locid);

#ifdef __cplusplus
}
#endif

#endif /* CS2_TYPES_H */
