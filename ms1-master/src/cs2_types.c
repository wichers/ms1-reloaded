/**
 * @file cs2_types.c
 * @brief CS2 CAN ID encode/decode, hash, CRC-16, and Loc-ID helpers.
 */

#include "cs2_types.h"

/* ========================================================================
 * CAN ID encode/decode
 * ====================================================================== */

uint32_t cs2_id_encode(const cs2_can_id_t *id)
{
    uint32_t can_id = 0;
    can_id |= ((uint32_t)(id->priority & CS2_ID_PRIO_MASK)) << CS2_ID_PRIO_SHIFT;
    can_id |= ((uint32_t)(id->command  & CS2_ID_CMD_MASK))  << CS2_ID_CMD_SHIFT;
    can_id |= ((uint32_t)(id->response & CS2_ID_RESP_MASK)) << CS2_ID_RESP_SHIFT;
    can_id |= ((uint32_t)(id->hash     & CS2_ID_HASH_MASK)) << CS2_ID_HASH_SHIFT;
    return can_id;
}

void cs2_id_decode(uint32_t can_id, cs2_can_id_t *id)
{
    id->priority = (uint8_t)((can_id >> CS2_ID_PRIO_SHIFT) & CS2_ID_PRIO_MASK);
    id->command  = (uint8_t)((can_id >> CS2_ID_CMD_SHIFT)  & CS2_ID_CMD_MASK);
    id->response = (uint8_t)((can_id >> CS2_ID_RESP_SHIFT) & CS2_ID_RESP_MASK);
    id->hash     = (uint16_t)((can_id >> CS2_ID_HASH_SHIFT) & CS2_ID_HASH_MASK);
}

/* ========================================================================
 * Hash calculation
 * ====================================================================== */

uint16_t cs2_hash_from_uid(uint32_t uid)
{
    uint16_t hash = (uint16_t)(uid >> 16) ^ (uint16_t)(uid & 0xFFFF);
    /* Fix bits 9,8,7 to 1,1,0 (value 6 << 7 = 0x0300) for CS1 compat */
    hash = (hash & CS2_HASH_CLEAR_MASK) | CS2_HASH_SET_BITS;
    return hash;
}

/* ========================================================================
 * CRC-16/CCITT
 * ====================================================================== */

uint16_t cs2_crc16(const uint8_t *data, uint32_t length)
{
    uint16_t crc = CS2_CRC_INIT;
    for (uint32_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ CS2_CRC_POLY;
            } else {
                crc = crc << 1;
            }
        }
    }
    return crc;
}

/* ========================================================================
 * Loc-ID helpers
 * ====================================================================== */

uint32_t cs2_locid_encode(uint16_t address, cs2_protocol_t protocol)
{
    uint16_t base;
    switch (protocol) {
        case CS2_PROTO_MM:  base = CS2_LOCID_MM_BASE;  break;
        case CS2_PROTO_MFX: base = CS2_LOCID_MFX_BASE; break;
        case CS2_PROTO_DCC: base = CS2_LOCID_DCC_BASE; break;
        case CS2_PROTO_SX1: base = CS2_LOCID_SX1_BASE; break;
        default:            base = 0;                   break;
    }
    return (uint32_t)(base + address);
}

uint16_t cs2_locid_address(uint32_t locid)
{
    uint16_t addr = (uint16_t)(locid & 0xFFFF);
    if (addr >= CS2_LOCID_DCC_BASE) {
        return addr - CS2_LOCID_DCC_BASE;
    } else if (addr >= CS2_LOCID_MFX_BASE) {
        return addr - CS2_LOCID_MFX_BASE;
    } else if (addr >= CS2_LOCID_SX1_BASE && addr <= CS2_LOCID_SX1_END) {
        return addr - CS2_LOCID_SX1_BASE;
    } else {
        return addr - CS2_LOCID_MM_BASE;
    }
}

cs2_protocol_t cs2_locid_protocol(uint32_t locid)
{
    uint16_t addr = (uint16_t)(locid & 0xFFFF);
    if (addr >= CS2_LOCID_DCC_BASE) {
        return CS2_PROTO_DCC;
    } else if (addr >= CS2_LOCID_MFX_BASE) {
        return CS2_PROTO_MFX;
    } else if (addr >= CS2_LOCID_SX1_BASE && addr <= CS2_LOCID_SX1_END) {
        return CS2_PROTO_SX1;
    } else {
        return CS2_PROTO_MM;
    }
}
