/*==========================================================================
 * signal/mfx_register.c - Finding, registering and reading mfx decoders (0x4A90-0x5951)
 *
 * mfx locomotives answer on the track (feedback, captured by timer channel 0 -
 * signal/feedback_capture.c - and evaluated by mfx_feedback_bit/mfx_feedback_data). The engine, one
 * step per call from the scheduler, works in states (mfx_state):
 *   1      search: is there an unregistered decoder?
 *   2      find its 32-bit UID bit by bit (mfx_substate = bits known)
 *   3, 4   give it an address (3: the address of its slot, 4: a new one, mfx_next_address)
 *   5      check that a registered decoder (round robin over the slots) still answers
 *   6-17   read its configuration (mfx_cv, mfx_read_len bytes per read) into the slot:
 *          name, functions and their icons, speeds, the blocks they are in
 * A command takes three steps (mfx_cmd_steps counts them down): the frame, five sync patterns,
 * the feedback window, evaluated on a later step; in states 3 and 4 the address assignment
 * frame goes out three times instead, without a window.
 *
 * Functions:
 *   mfx_registration_start   (0x4A90) - continue with the UID kept in EEPROM
 *   mfx_icon_lookup          (0x4AD9) - mfx function icon -> MS1 icon
 *   mfx_registration_abort   (0x4AFA) - give up the decoder in mfx_slot
 *   mfx_reassign   (0x4B5C) - state 4 with the next new address
 *   mfx_set_state            (0x4B7B)
 *   mfx_next_state           (0x4B85)
 *   mfx_registration_process (0x4B8F) - one step
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/eeprom_config.h"
#include "../include/rom_tables.h"
#include "../include/track_cmd.h"
#include "../include/ui.h"
#include "../include/lokstack.h"


/* {mfx icon, MS1 icon} pairs */
static const u8 mfx_icon_map[18][2] = {
    { 0x01, 0x10 },
    { 0x02, 0x0B },
    { 0x03, 0x01 },
    { 0x04, 0x0E },
    { 0x05, 0x0F },
    { 0x06, 0x0B },
    { 0x07, 0x0C },
    { 0x08, 0x0D },
    { 0x09, 0x0C },
    { 0x0A, 0x07 },
    { 0x0B, 0x08 },
    { 0x20, 0x06 },
    { 0x21, 0x0A },
    { 0x22, 0x09 },
    { 0x23, 0x02 },
    { 0x24, 0x03 },
    { 0x25, 0x04 },
    { 0x26, 0x05 },
};

/* The command codes of a locomotive's drive and function frames (mfx_drive_cmd,
 * mfx_function_cmd), by mfx_substate / mfx_block02_cas when the readout finds them */
static const u8 mfx_cmd_codes[17] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x15, 0x16, 0x17, 0x18, 0x19,
};


void mfx_registration_start(void)
{
    mfx_search_flag = 1;
    mfx_state = 2;
    mfx_substate = 0x20;
    mfx_uid_lo = EE_U16(EE_MFX_UID + 2);
    mfx_uid_hi = EE_U16(EE_MFX_UID);
    if (EE_U16(EE_MFX_UID) == 0xFFFF && EE_U16(EE_MFX_UID + 2) == 0xFFFF)
        mfx_substate = 0x1C;
    system_status_flags |= 4;
    mcan_status_pending |= 4;
    timer_capture_ptr = 1;
}

u16 mfx_icon_lookup(u16 icon)
{
    u16 i;

    for (i = 0; (s16)i <= 0x11; i++)
        if (mfx_icon_map[i][0] == icon)
            return mfx_icon_map[i][1];
    return 0;
}

void mfx_registration_abort(void)
{
    u16 s;

    mfx_cmd_steps = 0;
    mfx_state = 0;
    mfx_uid_lo = 0;
    mfx_uid_hi = 0;
    ee_save_pending |= 0x400;
    s = mfx_slot;
    lok_records[s].unused = 0xFF;
    lok_states[s].flags |= LOKS_DELETED;
    HI8(lok_states[s].can_pending) |= 0x10;
    HI8(lok_states[s].track_pending) |= 0x10;
    HI8(lok_states[s].save_pending) |= 0x10;
    if ((s16)(s8)lok_current_slot == (s16)s) {
        lok_release_current();
        ui_enter_mode(1);
    }
}

void mfx_reassign(void)
{
    mfx_cmd_steps = 3;
    mfx_state = 4;
    if (--mfx_next_address <= 0xFF)
        mfx_next_address = 0x36AF;
}

void mfx_set_state(u8 state)
{
    mfx_state = state;
    mfx_substate = 0;
    mfx_block = 0;
}

void mfx_next_state(void)
{
    mfx_set_state(mfx_state + 1);
}

/* state 2 finished: the 32-bit UID is known - register it in its slot, or in a free one.
 * Returns the slot's entry, which the caller goes on with (e when there is no slot). */
static lok_record_t *register_uid(lok_record_t *e)
{
    u8 known = 0, flags = 0;
    u16 addr = 0;
    u8 s;
    lok_record_t *q;
    lok_state_t *d;
    u8 *p;
    u16 i;

    mfx_slot = 0;
    for (s = 0, q = lok_records; s <= 10; s++, q++)
        if (q->unused == 0 && q->protocol == LOK_MFX && q->mfx_uid_hi == mfx_uid_hi && q->mfx_uid_lo == mfx_uid_lo)
            break;
    mfx_slot = s;
    if (s <= 9) {
        /* a locomotive this MS1 knows: keep its state */
        d = &lok_states[s];
        flags = d->flags & 0x87;
        addr = d->controller;
        known = 1;
    } else {
        mfx_slot = 0;
        for (s = 0, q = lok_records, d = lok_states; s <= 10; s++, q++, d++)
            if (q->unused != 0 && !(d->can_pending & 0x1000) && !(d->track_pending & 0x1000) && !(d->save_pending & 0x1000))
                break;
        mfx_slot = s;
    }
    if (s > 9) {
        mfx_cmd_steps = 0;
        return e;
    }
    e = &lok_records[(u8)s];
    for (p = (u8 *)e, i = sizeof(lok_record_t); i != 0; i--)
        *p++ = 0;
    s = mfx_slot;
    if (++lok_states[s].change_count == 0)
        lok_states[s].change_count = 1;
    e->protocol = LOK_MFX;
    e->address = mfx_slot + 1;
    e->mfx_uid_lo = mfx_uid_lo;
    e->mfx_uid_hi = mfx_uid_hi;
    e->mfx_read_width = 1;
    lok_state_clear(mfx_slot);
    d = &lok_states[mfx_slot];
    d->object = mfx_slot + 0x200;
    d->controller = addr;
    d->flags = flags;
    mfx_state = 3;
    ee_save_pending |= 0x400;
    if (known)
        return e;
    if (operating_mode == 1 || (operating_mode == 3 && menu_top == 1 && menu_item == 0)) {
        if ((s8)lok_current_slot >= 0)
            lok_release_current();
        lok_current_slot = mfx_slot;
        ui_enter_mode(1);
    }
    return e;
}

/* the configuration read finished, or there is no more to read */
static void readout_done(void)
{
    ee_save_pending |= 0x400;
    mfx_uid_lo = 0;
    mfx_uid_hi = 0;
    mfx_cmd_steps = 0;
    HI8(lok_states[mfx_slot].can_pending) |= 8;
    HI8(lok_states[mfx_slot].save_pending) |= 8;
}

/* a read of states 6-17 brought its bytes: store them, choose the next state */
static void readout_result(lok_record_t *e)
{
    u8 t, bc, v, ic, z;
    u16 x, y, q;

    switch (mfx_state) {
    case 6:
        mfx_next_state();
        if (mfx_read_buf[0] != 1)
            mfx_reassign();
        break;
    case 7:
        mfx_next_state();
        mfx_block01_cas = mfx_read_buf[0];
        break;
    case 8:
        if (mfx_substate > mfx_block01_cas)
            mfx_next_state();
        t = mfx_read_buf[0];
        if (t == 0x16)
            mfx_ca_symbol = mfx_cv;
        else if (t == 0x17)
            mfx_ca_blocks = mfx_cv + 1;
        else if (t == 0x18)
            e->mfx_name_cv = mfx_cv + 1;
        else if (t == 0x14)
            mfx_ca_protocol = mfx_cv;
        if (mfx_substate > mfx_block01_cas ||
            (mfx_ca_blocks != 0 && e->mfx_name_cv != 0 && mfx_ca_protocol != 0 && mfx_ca_symbol != 0))
            mfx_next_state();
        break;
    case 9:
        mfx_substate++;
        if (mfx_substate == 1) {
            if (mfx_read_buf[0] != 1)
                mfx_reassign();
        } else if (mfx_substate == 2) {
            e->mfx_write_syncs = mfx_read_buf[0] & 0x3F;
            if (mfx_read_buf[0] & 0x40)
                e->mfx_write_syncs = (u8)((mfx_read_buf[0] & 0x3F) << 2);
        } else {
            if (mfx_read_buf[0] & 0x0C)
                e->mfx_read_width = 2;
            mfx_next_state();
        }
        break;
    case 10:
        v = mfx_read_buf[1] & 7;
        e->symbol = v;
        if (v > 3 || !(mfx_read_buf[1] & 8))
            e->symbol = 0;
        e->kinds[0] = 0x80;
        e->kinds[1] = 0x82;
        if (mfx_read_buf[0] != 1)
            mfx_reassign();
        else
            mfx_next_state();
        break;
    case 11:                                    /* the name, two bytes per read */
        e->name[mfx_substate] = mfx_read_buf[0];
        mfx_substate++;
        if (mfx_read_buf[0] != 0 && mfx_substate <= 15) {
            e->name[mfx_substate] = mfx_read_buf[1];
            mfx_substate++;
            if (mfx_read_buf[1] != 0 && mfx_substate <= 15)
                break;
        }
        e->name[15] = 0;
        mfx_next_state();
        break;
    case 12:                                    /* the block table */
        bc = mfx_block;
        switch (bc) {
        case 0:
            t = mfx_read_buf[0];
            if (t == 0) {
                mfx_next_state();
                mfx_cv = 0;
            } else {
                mfx_block = bc + 1;
                mfx_cv = ((u16)t << 8) + 1;
            }
            x = mfx_cv;
            goto next_block;
        case 1:
            if (mfx_read_buf[1] == 1) {
                x = mfx_cv;
                v = (u8)(x >> 8);
                switch (mfx_read_buf[0]) {
                case 2:
                    mfx_block02 = v;
                    mfx_block = 2;
                    goto next_block;
                case 4:
                    mfx_block04 = v;
                    mfx_block = 3;
                    goto next_block;
                case 5:
                    mfx_block05 = v;
                    mfx_block = 4;
                    goto next_block;
                case 8:
                    mfx_block08 = v;
                    mfx_block = 5;
                    goto next_block;
                }
            }
            mfx_substate++;
            mfx_block = 0;
            if (mfx_substate > 0x3F)
                mfx_next_state();
            x = mfx_cv;
            goto next_block;
        case 2:
            mfx_block02_groups = mfx_read_buf[0];
            v = mfx_read_buf[0];
            if (v > 0x11) {
                mfx_block02_groups = 0x11;
                v = 0x11;
            }
            mfx_block02_cas = mfx_read_buf[1];
            if ((u16)v * mfx_block02_cas == 0)
                mfx_block02 = 0;
            break;
        case 3:
            v = mfx_read_buf[0];
            if (v > 0x7E)
                v = 0x7D;
            mfx_block04_cas = v;
            if (v == 0)
                mfx_block04 = 0;
            break;
        case 4:
            v = mfx_read_buf[0];
            if (v > 0x7E)
                v = 0x7D;
            mfx_block05_cas = v;
            if (v == 0)
                mfx_block05 = 0;
            break;
        case 5:
            mfx_block08_cas = mfx_read_buf[0];
            if (mfx_block08_cas == 0)
                mfx_block08 = 0;
            break;
        }
        x = mfx_cv;
    next_block:
        if ((x & 0x3F) <= 1)
            break;
        mfx_block = 0;
        mfx_substate++;
        if (mfx_substate > 0x3F)
            mfx_next_state();
        break;
    case 13:                                    /* functions and their icons */
        bc = mfx_block;
        if (bc == 0) {
            t = mfx_read_buf[0];
            if (t == 0x10 && mfx_block05 != 0 && e->mfx_drive_cmd == 0) {
                mfx_block = 1;
                mfx_cv = (((u16)mfx_block05 << 2) + mfx_read_buf[1]) << 6;
            } else {
                if (t == 0x11 && mfx_block04 != 0 && e->mfx_function_cmd == 0) {
                    e->mfx_function_cmd = mfx_cmd_codes[mfx_substate / mfx_block02_cas];
                    mfx_ca_functions = mfx_cv;
                }
                mfx_substate++;
            }
        } else {
            if ((s8)bc < 0) {
                mfx_block = bc + 0x81;
                mfx_cv += 0x3F;
                if (mfx_read_buf[0] == 1) {
                    e->speed_object = 1;
                    e->mfx_drive_cmd = mfx_cmd_codes[mfx_substate / mfx_block02_cas];
                } else {
                    mfx_block = 0x7F;
                }
            } else {
                mfx_block = bc + 1;
                y = mfx_cv;
                mfx_cv = y + 0x40;
                t = mfx_read_buf[0];
                if (t == 0x10) {
                    mfx_block = bc + 0x80;
                    mfx_cv = y + 1;
                } else if (t == 0x17) {
                    e->mfx_vmax_cv = y;
                } else if (t == 0x13) {
                    e->mfx_accel_cv = y;
                }
            }
            z = e->mfx_drive_cmd;
            if ((s16)mfx_block05_cas < (s16)(mfx_block & 0x7F) ||
                (z != 0 && e->mfx_vmax_cv != 0 && e->mfx_accel_cv != 0)) {
                if (z == 0) {
                    e->mfx_vmax_cv = 0;
                    e->mfx_accel_cv = 0;
                }
                mfx_substate++;
                mfx_block = 0;
            }
        }
        q = (u16)mfx_block02_cas * mfx_block02_groups;
        if ((s16)q <= (s16)mfx_substate || (e->mfx_function_cmd != 0 && e->mfx_drive_cmd != 0))
            mfx_next_state();
        break;
    case 14:
        e->vmax = mfx_read_buf[0];
        mfx_next_state();
        break;
    case 15:
        e->accel = mfx_read_buf[0];
        e->decel = mfx_read_buf[1];
        mfx_next_state();
        break;
    case 16:                                    /* function mappings */
        bc = mfx_block;
        if (bc == 0) {
            t = mfx_read_buf[0];
            if (t == 0) {
                mfx_substate++;
            } else {
                mfx_block = 1;
                mfx_cv = (((u16)mfx_block04 << 2) + t) << 6;
            }
        } else if ((s8)bc >= 0) {
            if (mfx_read_buf[0] == 0x10) {
                mfx_cv++;
                mfx_block = bc + 0x80;
            } else {
                mfx_cv += 0x40;
                mfx_block = bc + 1;
                if (mfx_block > mfx_block04_cas) {
                    mfx_substate++;
                    mfx_block = 0;
                }
            }
        } else {
            v = mfx_read_buf[1];
            if (v != 0 || (mfx_read_buf[0] & 0x1F) != 0) {
                ic = (u8)mfx_icon_lookup(v);
                if (ic == 0)
                    ic = (u8)mfx_icon_lookup(mfx_read_buf[0] & 0x1F);
                if (ic == 0)
                    ic = 11;
                if ((s8)mfx_read_buf[0] < 0)
                    ic |= 0x80;
                e->functions[mfx_substate].icon = ic;
                lok_assign_functions(mfx_slot);
            }
            mfx_block = 0;
            mfx_substate++;
        }
        if (mfx_substate > 15)
            mfx_next_state();
        break;
    case 17:
        if (mfx_read_buf[0] == 0x10) {
            e->mfx_volume_cv = mfx_cv;
            e->volume = mfx_read_buf[1];
            HI8(e->protocol_flags) |= 0x10;
            mfx_next_state();
        } else if (++mfx_substate >= mfx_block08_cas) {
            mfx_next_state();
        }
        break;
    }
}

/* where the next read of the state's data is */
static void readout_address(lok_record_t *e)
{
    u16 x, y;
    u8 fa;

    mfx_read_len = 1;
    switch (mfx_state) {
    case 4:
        return;
    case 7:
        mfx_cv = 5;
        return;
    case 8:
        mfx_substate++;
        mfx_cv = (mfx_substate << 6) & 0x3FC0;
        return;
    case 9:
        y = mfx_ca_protocol;
        x = mfx_substate == 0 ? y + 1 : mfx_substate == 1 ? y + 4 : y + 5;
        mfx_cv = x;
        if (y != 0)
            return;
        e->mfx_write_syncs = 0xFF;
        mfx_next_state();
        /* fall through */
    case 10:
        y = mfx_ca_symbol;
        mfx_cv = y + 1;
        mfx_read_len = 2;
        if (y != 0)
            return;
        mfx_reassign();
        /* fall through */
    case 11:
        mfx_cv = e->mfx_name_cv + mfx_substate;
        mfx_read_len = 2;
        if (e->mfx_name_cv != 0)
            return;
        mfx_next_state();
        /* fall through */
    case 12:
        switch (mfx_block) {
        case 0:
            x = mfx_ca_blocks;
            mfx_cv = x + mfx_substate;
            break;
        case 2:
            mfx_cv += 3;
            /* fall through */
        case 1:
            mfx_read_len = 2;
            x = mfx_ca_blocks;
            break;
        case 3:
        case 4:
        case 5:
            mfx_cv += 4;
            x = mfx_ca_blocks;
            break;
        default:
            x = mfx_ca_blocks;
            break;
        }
        if (x == 0)
            mfx_reassign();
        return;
    case 13:
        fa = mfx_block02;
        if (mfx_block == 0) {
            mfx_cv = ((((u16)fa << 2) + mfx_substate) << 6) + 0x40;
            mfx_read_len = 2;
        }
        if (fa == 0)
            mfx_reassign();
        return;
    case 14:
        if (e->mfx_vmax_cv != 0) {
            mfx_cv = e->mfx_vmax_cv + 2;
            return;
        }
        mfx_next_state();
        /* fall through */
    case 15:
        if (e->mfx_accel_cv != 0) {
            mfx_read_len = 2;
            mfx_cv = e->mfx_accel_cv + 1;
            return;
        }
        mfx_next_state();
        /* fall through */
    case 16:
        if (mfx_block04 != 0 && e->mfx_function_cmd != 0) {
            if (mfx_block == 0)
                mfx_cv = mfx_ca_functions + mfx_substate + 1;
            else if ((s8)mfx_block < 0)
                mfx_read_len = 2;
            return;
        }
        mfx_next_state();
        /* fall through */
    case 17:
        if (mfx_block08 != 0) {
            mfx_read_len = 2;
            mfx_cv = ((((u16)mfx_block08 << 2) + mfx_substate) << 6) + 0x40;
            return;
        }
        mfx_next_state();
        readout_done();
        return;
    default:
        readout_done();
        return;
    }
}

/* 0x4B8F: returns 1 when a command went into the command buffer */
u16 mfx_registration_process(void)
{
    lok_record_t *e = &lok_records[(u8)mfx_slot];
    lok_record_t *rec;
    u8 st = mfx_state;
    u8 r, s2, start, s, cur, f;
    u16 x, cap;
    s16 n;
    lok_state_t *d;

    if (st > 4 && (e->unused != 0 || e->protocol != LOK_MFX) && timer_capture_mode == 0 && timer_capture_ptr != 0xFFFF) {
        timer_capture_ptr = 0;
        mfx_set_state(1);
        st = mfx_state;
    }

    if ((u8)(st - 3) <= 1) {
        if (mfx_cmd_steps == 0) {
            if (st == 3) {
                /* the decoder took its address: read its configuration */
                mfx_cmd_steps = 3;
                mfx_state = 6;
                mfx_ca_protocol = 0;
                mfx_ca_symbol = 0;
                mfx_ca_blocks = 0;
                mfx_read_offset = 0;
                mfx_block02 = 0;
                mfx_block04 = 0;
                mfx_block05 = 0;
                mfx_block08 = 0;
                mfx_read_len = 1;
                mfx_cv = 2;
            } else {
                /* a new address did not take: give up the slot */
                cur = lok_current_slot;
                s = mfx_slot;
                if ((s16)s == (s16)(s8)cur) {
                    lok_release_current();
                    s = mfx_slot;
                    cur = lok_current_slot;
                }
                lok_records[s].unused = 0xFF;
                lok_states[s].flags |= LOKS_DELETED;
                HI8(lok_states[s].can_pending) |= 0x10;
                HI8(lok_states[s].save_pending) |= 0x10;
                mfx_uid_lo = 0;
                mfx_uid_hi = 0;
                ee_save_pending |= 0x400;
                if ((s16)(s8)cur == (s16)s)
                    ui_enter_mode(1);
            }
        }
    } else if ((cap = timer_capture_ptr) != 0 && timer_capture_mode == 0 && cap != 0xFFFF) {
        /* the feedback of the last command. The pointer is read once, before the mode: a
         * capture the signal interrupt starts in between leaves cap at 0xFFFF */
        mfx_cmd_steps = 3;
        switch (st) {
        case 1:
            if (mfx_feedback_bit(14) > 0) {
                system_status_flags |= 4;
                mcan_status_pending |= 4;
                mfx_substate = 1;
                mfx_state = 2;
            } else {
                system_status_flags &= ~4;
                mcan_status_pending |= 4;
                mfx_cmd_steps = 0;
            }
            break;
        case 2:
            f = mfx_search_flag;
            if (f != 0 || mfx_feedback_bit(14) > 0) {
                mfx_search_flag = 0;
                mfx_substate++;
                mfx_retry = 0;
                if (mfx_substate > 0x20)
                    e = register_uid(e);
            } else {
                /* no answer: the bit is the other value */
                u16 sh = (mfx_substate + 0x1F) & 0x1F;
                u32 m = 0x80000000UL >> sh;
                mfx_uid_lo ^= (u16)m;
                mfx_uid_hi ^= (u16)(m >> 16);
                if (++mfx_retry > 0x14)
                    mfx_cmd_steps = f;
            }
            break;
        case 3:
        case 4:
            break;
        case 5:
            d = &lok_states[mfx_slot];
            if (mfx_feedback_bit(14) > 0) {
                if (d->flags & 0x20) {
                    d->flags &= ~0x20;
                    LO8(d->can_pending) |= 4;
                }
                d->timer = 0;
            } else if (++d->timer > 8) {
                d->timer = 9;
                if (!(d->flags & 0x20)) {
                    d->flags |= 0x20;
                    LO8(d->can_pending) |= 4;
                }
            }
            mfx_cmd_steps = 0;
            break;
        default:                                /* 0, 6-17: bytes read */
            n = (mfx_read_len == 1 || e->mfx_read_width == 1) ? 1 : 2;
            if (mfx_feedback_data(&mfx_read_buf[mfx_read_offset], n) <= 0) {
                if (++mfx_retry > 0x13)
                    mfx_registration_abort();
                break;
            }
            mfx_retry = 0;
            mfx_read_offset += (mfx_read_len == 1 || e->mfx_read_width == 1) ? 1 : 2;
            if (mfx_read_offset < mfx_read_len)
                break;
            mfx_read_offset = 0;
            readout_result(e);
            readout_address(e);
            break;
        }
        timer_capture_ptr = 0;
    }

    /* the next command */
    if (timer_capture_ptr == 0 && mfx_cmd_steps == 0) {
        mfx_cmd_steps = 3;
        mfx_substate = 0;
        mfx_retry = 0;
        mfx_uid_lo = 0;
        mfx_uid_hi = 0;
        if (mfx_state == 1) {
            /* check the registered mfx locomotives in turn */
            start = s = mfx_slot;
            do {
                s = (u8)(((s16)s + 1) % 10);
                mfx_slot = s;
                rec = &lok_records[s];
                if (rec->unused == 0 && rec->protocol == LOK_MFX) {
                    mfx_state = 5;
                    e = &lok_records[(u8)s];
                    break;
                }
            } while (s != start);
        } else {
            mfx_state = 1;
        }
    }
    r = mfx_cmd_steps;
    if (r == 0)
        return 0;
    s2 = mfx_state;
    if ((u8)(s2 - 3) <= 1) {
        x = s2 == 3 ? e->address : mfx_next_address;
        mfx_frame_assign(x, mfx_uid_hi, mfx_uid_lo);
    } else if (r == 3) {
        if (s2 == 1 || s2 == 2)
            mfx_frame_search(mfx_substate, mfx_uid_hi, mfx_uid_lo);
        else if (s2 == 5)
            mfx_frame_ping(e->address, e->mfx_uid_hi, e->mfx_uid_lo);
        else
            mfx_frame_read_cv(e->address, mfx_cv + mfx_read_offset,
                                 (mfx_read_len == 1 || e->mfx_read_width == 1) ? 1 : 2);
    } else if (r == 2) {
        mfx_cmd_syncs(5);
    } else {
        if (s2 == 1 || s2 == 2 || s2 == 5)
            mfx_cmd_feedback(14, 2);
        else
            mfx_cmd_feedback((mfx_read_len == 1 || e->mfx_read_width == 1) ? 0x2E : 0x36, 1);
        timer_capture_ptr = 0xFFFF;
    }
    mfx_cmd_steps--;
    return 1;
}
