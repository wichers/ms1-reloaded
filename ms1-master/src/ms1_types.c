/**
 * @file ms1_types.c
 * @brief M-CAN CAN ID encoding/decoding implementations.
 */

#include "ms1_types.h"

/* ========================================================================
 * Registration CAN ID
 * ====================================================================== */

uint32_t ms1_id_encode_reg(const ms1_reg_id_t *id)
{
    /* Build the 16-bit handle from registration sub-fields */
    uint16_t handle = 0;
    handle |= (uint16_t)(id->uid_byte  & MS1_REG_UID_MASK)   << MS1_REG_UID_SHIFT;
    handle |= (uint16_t)(id->stage     & MS1_REG_STAGE_MASK)  << MS1_REG_STAGE_SHIFT;
    handle |= (uint16_t)(id->direction & MS1_REG_DIR_MASK)    << MS1_REG_DIR_SHIFT;

    /* Encode into the operational CAN ID layout: prio | handle | cmd=0 | node */
    uint32_t can_id = 0;
    can_id |= ((uint32_t)(id->priority & MS1_REG_PRIO_MASK)) << MS1_REG_PRIO_SHIFT;
    can_id |= ((uint32_t)(handle & MS1_REG_HANDLE_MASK))      << MS1_REG_HANDLE_SHIFT;
    /* command bits 9-7 are always 0 for registration */
    can_id |= ((uint32_t)(id->node & MS1_REG_NODE_MASK))      << MS1_REG_NODE_SHIFT;
    return can_id;
}

void ms1_id_decode_reg(uint32_t can_id, ms1_reg_id_t *id)
{
    /* Decode CAN ID level fields (same as operational layout) */
    id->priority  = (uint8_t)((can_id >> MS1_REG_PRIO_SHIFT) & MS1_REG_PRIO_MASK);
    id->node      = (uint8_t)((can_id >> MS1_REG_NODE_SHIFT) & MS1_REG_NODE_MASK);

    /* Extract the 16-bit handle */
    uint16_t handle = (uint16_t)((can_id >> MS1_REG_HANDLE_SHIFT) & MS1_REG_HANDLE_MASK);

    /* Decode handle sub-fields */
    id->uid_byte  = (uint8_t)((handle >> MS1_REG_UID_SHIFT)   & MS1_REG_UID_MASK);
    id->stage     = (uint8_t)((handle >> MS1_REG_STAGE_SHIFT) & MS1_REG_STAGE_MASK);
    id->direction = (uint8_t)((handle >> MS1_REG_DIR_SHIFT)   & MS1_REG_DIR_MASK);
}

/* ========================================================================
 * Operational CAN ID
 * ====================================================================== */

uint32_t ms1_id_encode_op(const ms1_op_id_t *id)
{
    uint32_t can_id = 0;
    can_id |= ((uint32_t)(id->priority & MS1_OP_PRIO_MASK))   << MS1_OP_PRIO_SHIFT;
    can_id |= ((uint32_t)(id->handle   & MS1_OP_HANDLE_MASK)) << MS1_OP_HANDLE_SHIFT;
    can_id |= ((uint32_t)(id->command  & MS1_OP_CMD_MASK))    << MS1_OP_CMD_SHIFT;
    can_id |= ((uint32_t)(id->node     & MS1_OP_NODE_MASK))   << MS1_OP_NODE_SHIFT;
    return can_id;
}

void ms1_id_decode_op(uint32_t can_id, ms1_op_id_t *id)
{
    id->priority = (uint8_t)((can_id >> MS1_OP_PRIO_SHIFT)   & MS1_OP_PRIO_MASK);
    id->handle   = (uint16_t)((can_id >> MS1_OP_HANDLE_SHIFT) & MS1_OP_HANDLE_MASK);
    id->command  = (uint8_t)((can_id >> MS1_OP_CMD_SHIFT)    & MS1_OP_CMD_MASK);
    id->node     = (uint8_t)((can_id >> MS1_OP_NODE_SHIFT)   & MS1_OP_NODE_MASK);
}

/* ========================================================================
 * Flash CAN ID
 * ====================================================================== */

uint32_t ms1_id_encode_flash(const ms1_flash_id_t *id)
{
    uint32_t can_id = 0;
    can_id |= ((uint32_t)(id->priority     & MS1_OP_PRIO_MASK))       << MS1_FLASH_PRIO_SHIFT;
    can_id |= ((uint32_t)(id->flash_handle & MS1_FLASH_HANDLE_MASK))  << MS1_FLASH_HANDLE_SHIFT;
    can_id |= ((uint32_t)(id->request      & MS1_FLASH_REQ_MASK))     << MS1_FLASH_REQ_SHIFT;
    can_id |= ((uint32_t)(id->stage        & MS1_FLASH_STAGE_MASK))   << MS1_FLASH_STAGE_SHIFT;
    can_id |= ((uint32_t)(id->flags        & MS1_FLASH_FLAGS_MASK))   << MS1_FLASH_FLAGS_SHIFT;
    can_id |= ((uint32_t)(id->node         & MS1_FLASH_NODE_MASK))    << MS1_FLASH_NODE_SHIFT;
    return can_id;
}

void ms1_id_decode_flash(uint32_t can_id, ms1_flash_id_t *id)
{
    id->priority     = (uint8_t)((can_id >> MS1_FLASH_PRIO_SHIFT)   & MS1_OP_PRIO_MASK);
    id->flash_handle = (uint8_t)((can_id >> MS1_FLASH_HANDLE_SHIFT) & MS1_FLASH_HANDLE_MASK);
    id->request      = (uint8_t)((can_id >> MS1_FLASH_REQ_SHIFT)    & MS1_FLASH_REQ_MASK);
    id->stage        = (uint8_t)((can_id >> MS1_FLASH_STAGE_SHIFT)  & MS1_FLASH_STAGE_MASK);
    id->flags        = (uint8_t)((can_id >> MS1_FLASH_FLAGS_SHIFT)  & MS1_FLASH_FLAGS_MASK);
    id->node         = (uint8_t)((can_id >> MS1_FLASH_NODE_SHIFT)   & MS1_FLASH_NODE_MASK);
}
