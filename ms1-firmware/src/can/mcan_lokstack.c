/*==========================================================================
 * can/mcan_lokstack.c - the locomotive slots on the CAN bus (0x601A, 0x7642)
 *
 * mcan_lokstack_sync runs from mcan_process every pass:
 *   - As master (is_master), a locomotive completed in the edit record
 *     (lok_edit, its unused 0) moves into the first free slot and becomes the current one
 *     (operating mode 1, or the menu 1 with item other than 1); with no free slot "LISTE
 *     VOLL" shows for 2 s (show_message).
 *   - Each slot's changes, flagged in its state's can_pending (the functions in
 *     can_functions), go out as frames; a flag is cleared once mcan_send has queued its
 *     frame. A freed slot (unused nonzero, can_pending 0x1000) is announced with command
 *     0x1A.
 *   - As slave, the state's request steps through the requests to the master, one each
 *     time its timer runs out.
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/lokstack.h"
#include "../include/str.h"
#include "../include/mcan.h"
#include "../include/ui.h"

extern void show_message(u8 countdown, u8 mode);           /* 0xDCA9 */

#define W(p, o)   (*(u16 *)((u8 *)(p) + (o)))
#define PROTO     mcan_node

void mcan_lokstack_sync(void)
{
    u8 msg[8];
    lok_record_t *e, *r;
    lok_state_t *d;
    u8 *p, *q;
    s16 i, j, n;
    u16 x, mask, sent;
    u8 b, pos;

    if (is_master != 0 && lok_edit.unused == 0) {
        d = lok_states;
        r = lok_records;
        for (i = 0;;) {
            b = r->unused;
            r++;
            if (b != 0 && !(d->can_pending & 0x1000) && !(d->save_pending & 0x1000)
                && !(d->track_pending & 0x1000))
                break;
            i++;
            d++;
            if (i > 10)
                break;
        }
        if (i > 9) {
            show_message(2, 0x3E);
        } else {
            q = (u8 *)&lok_records[i];
            for (p = (u8 *)&lok_edit; p != (u8 *)&lok_edit + sizeof(lok_record_t); )
                *q++ = *p++;
            d = &lok_states[i];
            if (++d->change_count == 0)
                d->change_count = 1;
            lok_state_clear(i);
            d->object = i + 0x200;
            HI8(d->can_pending) |= 8;
            HI8(d->save_pending) |= 8;
            lok_check_conflict(i);
            lok_make_name_unique(i);
            if (operating_mode == 1 || (operating_mode == 3 && menu_top == 1 && menu_item != 1)) {
                if ((s8)lok_current_slot >= 0)
                    lok_release_current();
                lok_current_slot = (u8)i;
                ui_enter_mode(1);
            }
        }
        lok_edit.unused = 0xFF;
    }

    for (i = 0; i <= 9; i++) {
        e = &lok_records[i];
        d = &lok_states[i];

        if (e->unused != 0) {
            /* a freed slot */
            if (!(d->can_pending & 0x1000))
                continue;
            b = is_master;
            if (b != 0) {
                lok_recheck_conflicts();
                lok_recheck_conflicts();
                if (e->protocol == LOK_MFX) {
                    mfx_reg_counter++;
                    ee_save_pending |= 0x100;
                }
                b = is_master;
            }
            W(msg, 0) = 0;
            W(msg, 2) = b ? 0 : mcan_sd_handle;
            W(msg, 4) = d->object;
            W(msg, 6) = d->change_count;
            if (mcan_send(0x1A80 | PROTO, mcan_start_handle, msg, 8))
                HI8(d->can_pending) &= ~0x10;
            continue;
        }

        /* speed and direction */
        if (d->can_pending & 3) {
            msg[0] = e->speed_object;
            msg[1] = 0;
            msg[2] = d->speed;
            if (e->speed_object != 0 && mcan_send(0x900 | PROTO, d->object, msg, 3))
                LO8(d->can_pending) &= ~3;
        }
        /* the locomotive's address and flags */
        if (d->can_pending & 0x2004) {
            if (is_master != 0) {
                lok_check_conflict(i);
                lok_recheck_conflicts();
                lok_recheck_conflicts();
            }
            W(msg, 0) = 0x4001;
            W(msg, 2) = 0x100;
            W(msg, 4) = e->address;
            x = (d->flags & 0x20) ? 2 : 0;
            if (e->flags != 0)
                x++;
            W(msg, 6) = x;
            if (e->speed_object != 0
                && mcan_send(0x1880 | PROTO, d->object, msg, is_master ? 8 : 6)) {
                LO8(d->can_pending) &= ~4;
                HI8(d->can_pending) &= ~0x20;
            }
        }
        /* the name, from position byte 0x19, 4 characters per frame */
        x = d->can_pending;
        if (x & 8) {
            d->name_sent = 1;
            d->can_pending = x & ~8;
        }
        while (d->name_sent != 0) {
            pos = d->name_sent;
            x = str_len(e->name) - pos + 2;
            n = x <= 4 ? (s16)x : 4;
            if (n <= 0) {
                d->name_sent = 0;
                break;
            }
            W(msg, 0) = 0x200;
            W(msg, 2) = pos - 1;
            W(msg, 4) = *(u16 *)&e->name[pos - 1];
            W(msg, 6) = *(u16 *)&e->name[pos + 1];
            if (!mcan_send(0x1880 | PROTO, d->object, msg, n + 4))
                break;
            d->name_sent += (u8)n;
        }
        /* decoder parameters 0x20-0x23 */
        if (d->can_pending & 0x10) {
            msg[0] = e->speed_object;
            msg[1] = 2;
            W(msg, 2) = 0;
            msg[4] = e->vmax;
            if (e->speed_object != 0 && mcan_send(0x1900 | PROTO, d->object, msg, 5))
                LO8(d->can_pending) &= ~0x10;
        }
        if (d->can_pending & 0x20) {
            msg[0] = e->speed_object;
            msg[1] = 2;
            W(msg, 2) = 0x100;
            msg[4] = e->accel;
            if (e->speed_object != 0 && mcan_send(0x1900 | PROTO, d->object, msg, 5))
                LO8(d->can_pending) &= ~0x20;
        }
        if (d->can_pending & 0x40) {
            msg[0] = e->speed_object;
            msg[1] = 2;
            W(msg, 2) = 0x200;
            msg[4] = e->decel;
            if (e->speed_object != 0 && mcan_send(0x1900 | PROTO, d->object, msg, 5))
                LO8(d->can_pending) &= ~0x40;
        }
        if (LO8(d->can_pending) & 0x80) {
            W(msg, 0) = 0x4004;
            W(msg, 2) = 0;
            msg[4] = e->volume;
            if (mcan_send(0x1880 | PROTO, d->object, msg, 4))
                LO8(d->can_pending) &= ~0x80;
        }
        /* the controlling device */
        if (d->can_pending & 0x400) {
            if (is_master == 0) {
                W(msg, 0) = (d->flags & 2) ? 0x300 : 0x301;
                W(msg, 2) = mcan_sd_handle;
                W(msg, 4) = d->change_count;
                if (mcan_send(0x1400 | PROTO, d->object, msg, 6))
                    HI8(d->can_pending) &= ~4;
            } else {
                b = d->flags;
                if (b & 2) {
                    if (!(b & 4) || d->controller == 0) {
                        d->controller = mcan_sd_handle;
                        ee_save_pending |= 0x40;
                        d->flags = b & ~0x40;
                    }
                } else if (b & 4) {
                    d->controller = mcan_peer_handle;
                    d->flags = b & ~0x40;
                } else {
                    d->controller = 0;
                }
                W(msg, 0) = 0x300;
                W(msg, 2) = d->controller;
                W(msg, 4) = d->change_count;
                msg[6] = (d->flags >> 1) & 1;
                if (mcan_send(0x1400 | PROTO, d->object, msg, 7))
                    HI8(d->can_pending) &= ~4;
            }
        }
        /* the slot record */
        if (d->can_pending & 0x800) {
            msg[0] = e->kinds[0];
            msg[1] = 0;
            W(msg, 2) = d->controller;
            W(msg, 4) = d->object;
            W(msg, 6) = d->change_count;
            if (mcan_send(0x1A00 | PROTO, mcan_start_handle, msg, 8))
                HI8(d->can_pending) &= ~8;
        }
        if (d->can_pending & 0x4000) {
            W(msg, 2) = 0;
            W(msg, 0) = 0x4000;
            if (mcan_send(0x1880 | PROTO, d->object, msg, 4))
                HI8(d->can_pending) &= ~0x40;
        }
        /* functions: each by its object */
        if (d->can_functions != 0) {
            for (j = 0, mask = 1; j <= 15; j++, mask <<= 1) {
                if (!(d->can_functions & mask) || (b = e->functions[j].object) == 0)
                    continue;
                msg[0] = b;
                msg[1] = 0;
                msg[2] = (d->functions & mask) ? 1 : 0;
                if (mcan_send(0x900 | PROTO, d->object, msg, 3))
                    d->can_functions &= ~mask;
            }
        }
        /* slave: the requests to the master */
        if (is_master != 0 || d->request == 0)
            continue;
        if (d->timer != 0) {
            d->timer--;
            continue;
        }
        x = d->request;
        sent = 0;
        if (x & 0xFF00) {
            if ((x & 0xFF00) == 0x100) {
                msg[0] = LO8(d->request);
                msg[1] = 1;
                sent = mcan_send(0x1900 | PROTO, d->object, msg, 2);
            } else if ((x & 0xFF00) == 0x300) {
                msg[0] = LO8(d->request);
                msg[1] = 0;
                sent = mcan_send(0x1900 | PROTO, d->object, msg, 2);
            } else {
                W(msg, 2) = LO8(d->request);
                W(msg, 0) = 0x200;
                sent = mcan_send(0x1880 | PROTO, d->object, msg, 4);
            }
        } else {
            switch (x) {
            case 1:
                W(msg, 2) = 0;
                W(msg, 0) = 0x4003;
                sent = mcan_send(0x1880 | PROTO, d->object, msg, 4);
                break;
            case 2:
                W(msg, 2) = 0;
                W(msg, 0) = 0x4001;
                sent = mcan_send(0x1880 | PROTO, d->object, msg, 4);
                break;
            case 3:
                W(msg, 2) = 0x100;
                W(msg, 0) = 0x4001;
                sent = mcan_send(0x1880 | PROTO, d->object, msg, 4);
                break;
            case 4:
                d->request = 0x101;
                break;
            case 5:
            case 6:
            case 7:
                if (e->speed_object == 0) {
                    d->request = x + 1;
                } else {
                    msg[0] = e->speed_object;
                    msg[1] = 2;
                    W(msg, 2) = (x - 5) << 8;
                    sent = mcan_send(0x1900 | PROTO, d->object, msg, 4);
                }
                break;
            case 8:
                W(msg, 2) = 0;
                W(msg, 0) = 0x4004;
                sent = mcan_send(0x1880 | PROTO, d->object, msg, 4);
                break;
            case 9:
                W(msg, 0) = 0x300;
                sent = mcan_send(0x1800 | PROTO, d->object, msg, 2);
                break;
            case 10:
                d->request = 0x200;
                break;
            }
        }
        if (sent)
            d->timer = 0xFF;
    }
}


/* As master, a slave's request (0x601A), by mcan_rx_cmd:
 *   3  the object after data {kind, step}[, from id]: answered {kind, step, from, id,
 *      its change counter}, 4 bytes when there is none
 *   4  data 0x80/0x81: a slot for the slave's new locomotive (the first free one, the
 *      slave's device handle as controller if given); data 0x40: the "own controller"
 *      object 0x101 for the slave, if free
 *   5  {controller, slot, counter} hand-over: the slot is released when controller and
 *      counter still match, else its current ones are sent back; for 0x101 the object
 *      goes back to this device */
void mcan_master_request(void)
{
    u8 msg[8];
    lok_record_t *e, *r;
    lok_state_t *d;
    u8 *p;
    s16 i;
    u16 x, id, h, v;
    u8 dlc, b13;

    switch (mcan_rx_cmd) {
    case 3:
        dlc = mcan_rx_dlc;
        if (dlc != 4 && dlc != 2)
            return;
        x = dlc == 4 ? mcan_rx_data23 : 0;
        W(msg, 0) = W(can_rx_frame_buf, 4);
        W(msg, 2) = x;
        id = mcan_next_object(x, can_rx_frame_buf[4], can_rx_frame_buf[5]);
        W(msg, 4) = id;
        W(msg, 6) = mcan_object_change_count(id);
        mcan_send(0x1980, 1, msg, (mcan_object_class(id) != 0 && id != 0) ? 8 : 4);
        return;

    case 4:
        dlc = mcan_rx_dlc;
        if ((u8)(dlc - 1) > 1 && dlc != 4)
            return;
        h = 0;
        b13 = 0;
        if (dlc > 1) {
            b13 = can_rx_frame_buf[5];
            if (dlc > 3)
                h = mcan_rx_data23;
        }
        if (mcan_object_class(h) == 0)
            return;
        if (can_rx_frame_buf[4] == 0x40) {
            if (mcan_peer_handle != 0)
                return;
            v = mcan_sd_change_count + 1;
            if (v == 0)
                v = 1;
            msg[0] = 0x40;
            msg[1] = b13;
            W(msg, 2) = h;
            W(msg, 4) = 0x101;
            W(msg, 6) = v;
            if (mcan_send(0x1A00, 1, msg, 8)) {
                mcan_peer_handle = 0x101;
                mcan_sd_change_count = v;
            }
            return;
        }
        if ((u8)(can_rx_frame_buf[4] + 0x80) > 1)
            return;
        d = lok_states;
        r = lok_records;
        for (i = 0;;) {
            if (r->unused != 0 && !(d->can_pending & 0x1000) && !(d->save_pending & 0x1000)
                && !(d->track_pending & 0x1000))
                break;
            r++;
            i++;
            d++;
            if (i > 10)
                break;
        }
        if (i > 9)
            return;
        e = &lok_records[i];
        for (p = (u8 *)e; p != (u8 *)e + sizeof(lok_record_t); )
            *p++ = 0;
        e->address = 1;
        e->kinds[0] = can_rx_frame_buf[4];
        e->kinds[1] = 0x82;
        d = &lok_states[i];
        if (++d->change_count == 0)
            d->change_count = 1;
        lok_state_clear(i);
        if (h != 0) {
            d->controller = h;
            d->flags |= 4;
            HI8(d->can_pending) |= 4;
        }
        d->object = i + 0x200;
        HI8(d->can_pending) |= 8;
        HI8(d->save_pending) |= 8;
        lok_check_conflict(i);
        return;

    case 5:
        if (mcan_rx_dlc != 8 || mcan_rx_data23 != mcan_peer_handle)
            return;
        if (mcan_object_class(mcan_rx_data45) != 5) {
            if (mcan_rx_data45 != mcan_peer_handle)
                return;
            W(msg, 0) = 0;
            W(msg, 2) = 0;
            W(msg, 4) = 0x101;
            W(msg, 6) = mcan_sd_change_count;
            mcan_send(0x1A80, 1, msg, 8);
            mcan_slave_release_all();
            return;
        }
        i = lok_slot_of_object(mcan_rx_data45);
        if (mcan_object_controller(mcan_rx_data45) == mcan_rx_data23
            && mcan_object_change_count(mcan_rx_data45) == mcan_rx_data67) {
            lok_records[i].unused = 0xFF;
            d = &lok_states[i];
            d->flags |= 0x80;
            HI8(d->track_pending) |= 0x10;
            HI8(d->save_pending) |= 0x10;
            HI8(d->can_pending) |= 0x10;
            if ((s8)lok_current_slot == i)
                ui_enter_mode(1);
            return;
        }
        W(msg, 0) = 0x300;
        W(msg, 2) = mcan_object_controller(mcan_rx_data45);
        W(msg, 4) = mcan_object_change_count(mcan_rx_data45);
        mcan_send(0x1800, mcan_rx_data45, msg, 6);
        return;
    }
}
