/*==========================================================================
 * can/mcan_status.c - status and control frames (0x6A95, 0x6C73, 0x702B)
 *
 * A change of a system_status_flags bit is flagged in the same bit of mcan_status_pending
 * (bits 0-3); mcan_send_status sends a frame for each flagged bit whose object id
 * (status_id_*) is nonzero and clears the flag once the frame is queued. The frame:
 * command 0x900 | mcan_node, data {object id, 0, state}.
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/mcan.h"


#define STATUS_CMD  (0x900 | mcan_node)

/* mcan_status_pending bit 9 is an emergency stop request: as master, it clears every
 * locomotive's pending direction, function and event bits and sets STOP. The STOP frame
 * carries 2 + the STOP bit while the request is still flagged (as slave). */
void mcan_send_status(void)
{
    u8 msg[3];
    u16 handle, cms, n;
    lok_state_t *d;

    handle = is_master ? 2 : mcan_sy_handle;
    msg[1] = 0;
    msg[0] = mcan_status_idx_stop;
    if (is_master != 0) {
        cms = mcan_status_pending;
        if (cms & 0x200) {
            d = lok_states;
            for (n = 10; n != 0; n--, d++) {
                if (d->speed & 0x80)
                    d->track_pending ^= 2;
                d->speed = 0;
                d->can_functions = ((u16)HI8(d->functions) << 8) | LO8(d->functions);
                LO8(d->functions) = 0;
                HI8(d->functions) = 0;
                d->track_functions = 0;
                d->can_pending |= 3;
                d->track_pending &= ~1;
            }
            system_status_flags |= 1;
            mcan_status_pending = (cms & ~0x200) | 1;
        }
    }

    msg[2] = (system_status_flags & 1) ? 1 : 0;
    if (mcan_status_pending & 0x200)
        msg[2] += 2;
    if ((mcan_status_pending & 0x201) && mcan_status_idx_stop
        && mcan_send(STATUS_CMD, handle, msg, 3))
        mcan_status_pending &= ~0x201;

    msg[0] = mcan_status_idx_mfx_reg;
    msg[2] = (system_status_flags >> 2) & 1;
    if ((mcan_status_pending & 4) && mcan_status_idx_mfx_reg && mcan_send(STATUS_CMD, handle, msg, 3))
        mcan_status_pending &= ~4;

    msg[0] = mcan_status_idx_overload;
    msg[2] = (system_status_flags >> 1) & 1;
    if ((mcan_status_pending & 2) && mcan_status_idx_overload && mcan_send(STATUS_CMD, handle, msg, 3))
        mcan_status_pending &= ~2;

    msg[0] = mcan_status_idx_mm_prog;
    msg[2] = (system_status_flags >> 3) & 1;
    if ((mcan_status_pending & 8) && mcan_status_idx_mm_prog && mcan_send(STATUS_CMD, handle, msg, 3))
        mcan_status_pending &= ~8;
}

/* A received status frame (0x6C73): data {object id, 0, state} sets the state, data
 * {object id, 0} asks for it, data {object id, 1[, kind]} is about the object ids. As
 * slave: the master's STOP, overload, mfx and bit 3 states become ours; while
 * registering (mcan_reg_step 11-13, mcan_list_index the id asked for) the
 * master's answers name the status objects. As master: a query flags the state to send,
 * a STOP state with bit 1 is an emergency stop request, and an id query is answered
 * with the kind of each of the ids 1-4. */
void mcan_receive_status(void)
{
    u8 msg[3];
    u8 dlc, id, v;
    u16 idx;

    dlc = mcan_rx_dlc;
    if (dlc <= 1)
        return;
    id = can_rx_frame_buf[4];
    msg[0] = can_rx_frame_buf[4];
    msg[1] = can_rx_frame_buf[5];
    if (id == 0)
        return;

    if (can_rx_frame_buf[5] == 0) {
        if (dlc <= 2) {
            if (is_master == 0)
                return;
            if (id == mcan_status_idx_overload)
                mcan_status_pending |= 2;
            else if (id == mcan_status_idx_mfx_reg)
                mcan_status_pending |= 4;
            else if (id == mcan_status_idx_mm_prog)
                mcan_status_pending |= 8;
            else if (id == mcan_status_idx_stop)
                mcan_status_pending |= 1;
            return;
        }
        if (is_master == 0) {
            v = can_rx_frame_buf[6];
            if (id == mcan_status_idx_overload)
                system_status_flags = (v & 1) ? system_status_flags | 2 : system_status_flags & ~2;
            if (id == mcan_status_idx_mfx_reg)
                system_status_flags = (v & 1) ? system_status_flags | 4 : system_status_flags & ~4;
            if (id == mcan_status_idx_mm_prog)
                system_status_flags = (v & 1) ? system_status_flags | 8 : system_status_flags & ~8;
            if (mcan_reg_step == 12 && id == mcan_list_index) {
                mcan_reg_step = 11;
                mcan_list_index = id + 1;
                mcan_link_flags |= 1;
            }
        }
        if (id == mcan_status_idx_stop) {
            v = can_rx_frame_buf[6];
            system_status_flags = (v & 1) ? system_status_flags | 1 : system_status_flags & ~1;
            if (is_master != 0 && (v & 2))
                mcan_status_pending |= 0x201;
        }
        return;
    }
    if (can_rx_frame_buf[5] != 1)
        return;

    if (is_master != 0) {
        /* master: the kind of the object id asked for */
        if (dlc != 2)
            return;
        msg[2] = 2;
        if (id == 2) {
            mcan_send(0x1900, 2, msg, 3);
            mcan_status_pending |= 2;
        }
        msg[2] = 4;
        if (id == 4) {
            mcan_send(0x1900, 2, msg, 3);
            mcan_status_pending |= 4;
        }
        msg[2] = 3;
        if (id == 3) {
            mcan_send(0x1900, 2, msg, 3);
            mcan_status_pending |= 8;
        }
        msg[2] = 1;
        if (id == 1) {
            mcan_send(0x1900, 2, msg, 3);
            mcan_status_pending |= 1;
        }
        if (id > 4)
            mcan_send(0x1900, 2, msg, 2);
        return;
    }

    /* slave registration: the answer for the id asked for */
    idx = mcan_list_index;
    if (id != idx || mcan_reg_step != 11)
        return;
    if (dlc <= 2) {
        mcan_reg_step = 13;
        mcan_list_index = 0;
    } else {
        mcan_reg_step = 12;
        v = can_rx_frame_buf[6];
        if (v == 2 && mcan_status_idx_overload == 0)
            mcan_status_idx_overload = id;
        else if (v == 4 && mcan_status_idx_mfx_reg == 0)
            mcan_status_idx_mfx_reg = id;
        else if (v == 3 && mcan_status_idx_mm_prog == 0)
            mcan_status_idx_mm_prog = id;
        else if (v == 1 && mcan_status_idx_stop == 0)
            mcan_status_idx_stop = id;
        else {
            mcan_reg_step--;
            mcan_list_index = idx + 1;
        }
    }
    mcan_link_flags |= 1;
}

/* A received control frame (0x702B), data {3, 0 take | 1 release[, device handle]} for a
 * locomotive slot (mcan_rx_handle). As master: our own device taking or releasing it sets or
 * clears flags bit 2, and the slot's controlling device is announced (can_pending bit
 * 0x400, mcan_lokstack_sync). As slave, a take names the controlling device (controller),
 * advances the request state, and data 4 bit 0 tells whether it is this device. */
void mcan_receive_control(void)
{
    u8 sub;
    s16 s;
    lok_state_t *d;

    if (mcan_rx_dlc <= 1)
        return;
    sub = can_rx_frame_buf[5];
    if (can_rx_frame_buf[4] != 3)
        return;
    s = lok_slot_of_object(mcan_rx_handle);
    if (s > 9)
        return;
    d = &lok_states[s];
    if (sub == 1) {
        if (is_master == 0)
            return;
        if (mcan_rx_dlc > 3 && mcan_rx_data23 == mcan_peer_handle)
            d->flags &= ~4;
        HI8(d->can_pending) |= 4;
        return;
    }
    if (sub != 0)
        return;
    if (is_master != 0) {
        if (mcan_rx_dlc > 3 && mcan_rx_data23 == mcan_peer_handle)
            d->flags |= 4;
        HI8(d->can_pending) |= 4;
        return;
    }
    if (mcan_rx_dlc > 3) {
        d->controller = mcan_rx_data23;
        if (mcan_rx_data23 == 0 && (d->flags & 2))
            HI8(d->can_pending) |= 4;
        mcan_request_advance(d, 9);
    }
    if (mcan_rx_dlc <= 6)
        return;
    if (can_rx_frame_buf[10] & 1)
        d->flags |= 4;
    else
        d->flags &= ~4;
}
