/*==========================================================================
 * can/mcan_object.c - object requests and locomotive state (0x5F2E, 0x5FA2, 0x637A,
 *                      0x71C6, 0x71F6)
 *
 * A peer addresses an object by its handle (mcan_rx_handle): this device's own objects or a
 * locomotive slot (mcan_object_kind, mcan_object_class: 1-4 the device's,
 * 5 a slot). The request data {group, item, sub-item, offset[, value...]} reads (4 data
 * bytes) or writes (more) an item; the answer, command 0x18 with bit 7, repeats the first
 * four bytes and carries up to four bytes of the item from the offset.
 *   group 0     the object's kind
 *   group 1     identification: 0 kind, 1-5 flash ID words, 3/3 the product string
 *   group 2     the name (device: device_name; slot: the locomotive's name)
 *   group 0x40  locomotive data of a slot: 0 send it all again, 1/0 protocol and
 *               capabilities, 1/1 address and flags, 2 the controlling device, 3 symbol,
 *               4 parameter 0x23
 *   group 0x41  control state
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/lokstack.h"
#include "../include/str.h"
#include "../include/mcan.h"


#define FLASH_W(a)  (*(const u16 *)(a))
#define W(p, off)   (*(u16 *)((u8 *)(p) + (off)))
#define ANSWER_CMD  (0x1880 | mcan_node)

/* Send four characters of s from offset off: data {w0, b2, off, s[off..off+3]}, the
 * length cut at s's end (just {w0, b2, off, 0} past it). */
u16 mcan_send_string(const u8 *s, u8 off, u16 cmd, u16 handle, u16 w0, u8 b2)
{
    u8 msg[8];
    s16 len, n;
    const u8 *p;

    len = str_len(s);
    W(msg, 0) = w0;
    msg[2] = b2;
    msg[3] = off;
    p = s + off;
    W(msg, 6) = W(p, 2);
    W(msg, 4) = W(p, 0);
    n = len - off;
    if (n <= 0) {
        msg[4] = 0;
        n = 0;
    }
    n += 5;
    if (n > 8)
        n = 8;
    return mcan_send(cmd, handle, msg, n);
}

/* Send up to four bytes of the len bytes at p from offset off: data {w0, b2, off,
 * p[off..]}; 0xFFFF (nothing sent) when off is past the end. */
u16 mcan_send_data(const u8 *p, u8 off, u16 cmd, u16 handle, u16 w0, u8 b2, s16 len)
{
    u8 msg[8];
    s16 n;

    if ((s16)off >= len)
        return 0xFFFF;
    W(msg, 0) = w0;
    msg[2] = b2;
    msg[3] = off;
    p += off;
    W(msg, 6) = W(p, 2);
    W(msg, 4) = W(p, 0);
    n = len - off + 4;
    if (n > 8)
        n = 8;
    return mcan_send(cmd, handle, msg, n);
}

/* Copy n of the request's value bytes (data 4 on) into s from offset off. */
static void write_string(u8 *s, u8 off, s16 n)
{
    const u8 *src = &can_rx_frame_buf[8];
    u8 *dst = s + off;

    while (n-- > 0)
        *dst++ = *src++;
}

void mcan_object_request(void)
{
    u8 f[4];
    u8 flen, d0, d1, d2, d3, kind, dlc;
    u16 handle, w0, x;
    s16 s, len, n;
    lok_record_t *e;
    lok_state_t *d;

    if (mcan_rx_dlc <= 3)
        return;
    flen = 0;
    handle = mcan_rx_handle;
    d0 = can_rx_frame_buf[4];
    d1 = can_rx_frame_buf[5];
    d2 = can_rx_frame_buf[6];
    d3 = can_rx_frame_buf[7];
    kind = (u8)mcan_object_class(handle);
    if (kind == 0)
        return;
    w0 = ((u16)d0 << 8) | d1;

    switch (d0) {
    case 0:
        /* the object's kind */
        if (d1 != 0 || d2 != 0)
            break;
        switch (kind) {
        case 1: f[0] = 1; flen = 1; break;
        case 2: f[0] = 2; flen = 1; break;
        case 3: f[0] = 3; flen = 1; break;
        case 4: f[0] = 0x40; flen = 1; break;
        case 5:
            e = &lok_records[handle - 0x200];
            W(f, 2) = *(u16 *)&e->kinds[2];
            W(f, 0) = *(u16 *)&e->kinds[0];
            for (n = 0; n < 4 && f[n] != 0; n++)
                ;
            flen = n > 3 ? 3 : (u8)n;
            break;
        }
        break;
    case 1:
        /* identification */
        if (kind == 5)
            break;
        switch (d1) {
        case 0:
            if (d2 == 0) {
                W(f, 0) = 2;
                flen = 2;
            }
            break;
        case 1:
            if (d2 == 0) {
                W(f, 2) = FLASH_W(0xFF1A);
                W(f, 0) = FLASH_W(0xFF18);
                flen = 4;
            } else if (d2 == 1) {
                W(f, 2) = FLASH_W(0xFF16);
                W(f, 0) = FLASH_W(0xFF14);
                flen = 4;
            } else if (d2 == 2) {
                mcan_send_string((const u8 *)FLASH_W(0xFF50), d3, ANSWER_CMD, handle, w0, 2);
            }
            break;
        case 2:
            if (d2 == 0) {
                W(f, 2) = FLASH_W(0xFF26);
                W(f, 0) = FLASH_W(0xFF24);
                flen = 4;
            } else if (d2 == 1) {
                W(f, 2) = FLASH_W(0xFF2A);
                W(f, 0) = FLASH_W(0xFF28);
                flen = 4;
            }
            break;
        case 3:
            if (d2 == 0) {
                W(f, 2) = FLASH_W(0xF702);
                W(f, 0) = FLASH_W(0xF700);
                flen = 4;
            } else if (d2 == 1) {
                W(f, 2) = FLASH_W(0xF706);
                W(f, 0) = FLASH_W(0xF704);
                flen = 4;
            } else if (d2 == 2) {
                W(f, 2) = FLASH_W(0xF70A);
                W(f, 0) = FLASH_W(0xF708);
                flen = 4;
            } else if (d2 == 3) {
                mcan_send_string((const u8 *)FLASH_W(0xF720), d3, ANSWER_CMD, handle, w0, 3);
            }
            break;
        case 4:
            if (d2 == 0) {
                W(f, 2) = FLASH_W(0xF70E);
                W(f, 0) = FLASH_W(0xF70C);
                flen = 4;
            }
            break;
        case 5:
            if (d2 == 0) {
                W(f, 2) = FLASH_W(0xFF1E);
                W(f, 0) = FLASH_W(0xFF1C);
                flen = 4;
            } else if (d2 == 1) {
                W(f, 2) = FLASH_W(0xFF22);
                W(f, 0) = FLASH_W(0xFF20);
                flen = 4;
            }
            break;
        }
        break;
    case 2:
        /* the name */
        if (d1 != 0 || d2 != 0)
            break;
        dlc = mcan_rx_dlc;
        if (kind == 5) {
            s = lok_slot_of_object(handle);
            e = &lok_records[s];
            d = &lok_states[s];
            if (dlc > 4 && (is_master == 0
                            || (mcan_peer_handle != 0 && mcan_peer_handle == d->controller))) {
                /* write: as slave the name the master sent while asked for (request
                 * 0x200 + offset), the next part asked for while characters follow */
                len = str_len(e->name);
                n = 16 - d3;
                if (n > (s16)(dlc - 4))
                    n = dlc - 4;
                x = d->request;
                if (x - 0x200 == d3) {
                    if (n != 0 && can_rx_frame_buf[9] != 0) {
                        d->request = x + n;
                        d->timer = d1;
                    } else {
                        d->request = 0;
                    }
                }
                write_string(e->name, d3, n);
                e->name[15] = 0;
                n = d3 + dlc - 4;
                if (n > len && n <= 15)
                    e->name[n] = 0;
                LO8(d->save_pending) |= 8;
                if (is_master != 0)
                    LO8(d->can_pending) |= 8;
            }
            if (is_master != 0)
                mcan_send_string(e->name, d3, ANSWER_CMD, handle, w0, d2);
        } else if (kind == 4) {
            if (dlc > 4) {
                len = str_len(device_name);
                n = 16 - d3;
                if (n > (s16)(dlc - 4))
                    n = dlc - 4;
                write_string(device_name, d3, n);
                device_name[15] = 0;
                n = d3 + dlc - 4;
                if (n > len && n <= 15)
                    device_name[n] = 0;
                ee_save_pending |= 0x80;
                mcan_status_pending |= 0x80;
            }
            mcan_send_string(device_name, d3, ANSWER_CMD, handle, w0, d2);
        } else {
            mcan_send_string(kind == 1 ? (const u8 *)"Mobile Station Busmaster"
                             : kind == 2 ? (const u8 *)"Mobile Station Directory Service"
                             : (const u8 *)"Mobile Station Central",
                             d3, ANSWER_CMD, handle, w0, d2);
        }
        break;
    case 0x40:
        /* locomotive data */
        if (kind != 5 || d3 != 0)
            break;
        s = lok_slot_of_object(handle);
        e = &lok_records[s];
        d = &lok_states[s];
        if (is_master != 0 && d->controller != mcan_peer_handle)
            mcan_rx_dlc = 4;     /* not ours to change: a read */
        dlc = mcan_rx_dlc;
        switch (d1) {
        case 0:
            if (d2 == 0)
                HI8(d->track_pending) |= 0x40;
            break;
        case 1:
            if (e->kinds[0] == 0x82)
                break;
            if (d2 == 0) {
                if (dlc > 5) {
                    mcan_request_advance(d, 2);
                    e->protocol = can_rx_frame_buf[9];
                    if (mcan_rx_dlc > 7)
                        e->protocol_flags = mcan_rx_data67;
                }
                f[0] = 0;
                f[1] = e->protocol;
                W(f, 2) = e->protocol_flags;
                flen = 4;
            } else if (d2 == 1) {
                if (dlc > 5) {
                    mcan_request_advance(d, 3);
                    e->address = mcan_rx_data45;
                    if (mcan_rx_dlc > 7 && is_master == 0) {
                        x = mcan_rx_data67;
                        if (x & 1)
                            e->flags |= 1;
                        else
                            e->flags &= ~1;
                        if (x & 2)
                            d->flags |= 0x20;
                        else
                            d->flags &= ~0x20;
                    }
                    if (is_master != 0) {
                        LO8(d->can_pending) |= 4;
                        LO8(d->track_pending) |= 4;
                    }
                }
                W(f, 0) = e->address;
                x = (d->flags & 0x20) ? 2 : 0;
                if (e->flags & 1)
                    x++;
                W(f, 2) = x;
                flen = 4;
            }
            break;
        case 2:
            if (d2 == 0 && dlc > 7 && d->prog_cv == 0) {
                d->prog_value = can_rx_frame_buf[9];
                d->prog_cv = mcan_rx_data67;
                HI8(d->track_pending) |= 1;
            }
            break;
        case 3:
            if (d2 == 0) {
                if (dlc > 5)
                    e->symbol = can_rx_frame_buf[9];
                f[0] = 0;
                f[1] = e->symbol;
                mcan_request_advance(d, 1);
                flen = 2;
            }
            break;
        case 4:
            if (d2 == 0) {
                if (dlc > 5) {
                    mcan_request_advance(d, 8);
                    e->volume = can_rx_frame_buf[8];
                    if (is_master != 0) {
                        LO8(d->track_pending) |= 0x80;
                        LO8(d->can_pending) |= 0x80;
                    }
                }
                f[0] = 0;
                f[1] = e->volume;
                flen = 2;
            }
            break;
        }
        if (is_master == 0)
            flen = 0;
        break;
    case 0x41:
        /* control state */
        if (d1 == 0 && handle == mcan_sd_handle && d3 == 0) {
            W(f, 0) = (d2 == 0x80 || d2 == 0) ? 10 : 0;
            flen = 2;
        }
        break;
    }
    if (flen != 0)
        mcan_send_data(f, d3, ANSWER_CMD, handle, w0, d2, flen);
}

/* Answer the received request: data {data 0, data 1, value} (0x71C6). */
u16 mcan_answer_object(u16 value, u16 dlc)
{
    u8 msg[4];

    W(msg, 0) = W(can_rx_frame_buf, 4);
    W(msg, 2) = value;
    return mcan_send(0x1900, mcan_rx_handle, msg, dlc);
}

/* A received locomotive state frame (0x71F6) for a slot (mcan_rx_handle), data {object id,
 * sub, ...}. The object id is the slot's speed object or one of its function objects. A
 * frame too short to carry a value, or one for a slot this master does not control (the
 * state's controller), is a query: the master sends the
 * state (mcan_lokstack_sync). Otherwise:
 *   sub 0: speed object: data 2 the speed (bit 7 direction); function: data 2 bit 0 on
 *   sub 1: as master, a 2-byte query of an object's kind (0x101 speed, 0x200 + function
 *          kind, 0 / 2 bytes past the slot's objects); a write lists {2, kind} or 0x0101
 *          pairs naming objects as functions or the speed object
 *   sub 2: speed object, data 2 parameter 0-2, data 4 its value (bytes 0x20-0x22)
 * As slave each change advances the request state (mcan_request_advance). */
void mcan_receive_lok_state(void)
{
    lok_record_t *e;
    lok_state_t *d;
    u8 dlc, obj, sub, v, m, n;
    s16 i, j, k;
    u16 bit, x;

    dlc = mcan_rx_dlc;
    if (dlc <= 1)
        return;
    i = lok_slot_of_object(mcan_rx_handle);
    if (i > 9)
        return;
    e = &lok_records[i];
    d = &lok_states[i];
    obj = can_rx_frame_buf[4];
    sub = can_rx_frame_buf[5];
    if (obj == 0)
        return;

#define READ_ONLY(min)  (dlc <= (min) || (is_master != 0 \
                                          && d->controller != mcan_peer_handle))
    if (sub == 0) {
        if (obj == e->speed_object) {
            if (!READ_ONLY(2)) {
                mcan_request_advance(d, 0x300 | obj);
                v = can_rx_frame_buf[6];
                if (d->speed != v) {
                    x = d->track_pending | 1;
                    d->track_pending = x;
                    if ((d->speed ^ v) & 0x80)
                        d->track_pending = x ^ 2;
                    d->speed = can_rx_frame_buf[6];
                }
            }
            if (is_master != 0)
                LO8(d->can_pending) |= 3;
            return;
        }
        j = lok_function_of_object(e, obj);
        if (j > 15)
            return;
        if (!READ_ONLY(2)) {
            mcan_request_advance(d, 0x300 | obj);
            bit = 1 << j;
            if (can_rx_frame_buf[6] & 1)
                d->functions |= bit;
            else
                d->functions &= ~bit;
            d->track_functions |= 1 << j;
        }
        if (is_master != 0)
            d->can_functions |= 1 << j;
        return;
    }

    if (sub == 1) {
        if (!READ_ONLY(3)) {
            n = mcan_rx_dlc;
            for (k = 2; n > 3; ) {
                if (W(can_rx_frame_buf, 4 + k) == 0x0101 && e->speed_object == 0) {
                    mcan_request_advance(d, 0x100 | obj);
                    e->speed_object = obj;
                    HI8(d->save_pending) |= 8;
                    break;
                }
                if (can_rx_frame_buf[4 + k] == 2) {
                    j = lok_function_of_object(e, obj);
                    if (j > 15) {
                        for (j = 0; j <= 16 && e->functions[j].object != 0; j++)
                            ;
                    }
                    if (j <= 15) {
                        mcan_request_advance(d, 0x100 | obj);
                        e->functions[j].icon = can_rx_frame_buf[5 + k];
                        e->functions[j].object = obj;
                        HI8(d->save_pending) |= 8;
                        lok_assign_functions(i);
                    }
                    break;
                }
                k += 2;
                if (k + 2 > n)
                    break;
            }
        }
        m = is_master;
        if (m == 0) {
            if (mcan_rx_dlc == 2 && d->request - 0x100 == obj) {
                d->request = 5;
                d->timer = m;
            }
            return;
        }
        if (mcan_rx_dlc != 2)
            return;
        if (obj == e->speed_object) {
            mcan_answer_object(0x101, 4);
            return;
        }
        j = lok_function_of_object(e, obj);
        if (j <= 15)
            mcan_answer_object(e->functions[j].icon + 0x200, 4);
        else
            mcan_answer_object(0, (s16)lok_highest_object(e) < obj ? 2 : 4);
        return;
    }

    if (sub != 2 || dlc <= 3 || obj != e->speed_object || can_rx_frame_buf[7] != 0
        || can_rx_frame_buf[6] > 2)
        return;
    v = can_rx_frame_buf[6];
    if (!READ_ONLY(4)) {
        mcan_request_advance(d, 5 + v);
        (&e->vmax)[v] = can_rx_frame_buf[8];
        LO8(d->track_pending) |= 0x10 << v;
        LO8(d->save_pending) |= 0x10 << v;
    }
    if (is_master != 0)
        LO8(d->can_pending) |= 0x10 << v;
#undef READ_ONLY
}
