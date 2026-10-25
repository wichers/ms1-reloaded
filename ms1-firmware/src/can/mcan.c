/*==========================================================================
 * can/mcan.c - The MS1's CAN protocol: frames, registration, slots (0x5E70, 0x5EA5, 0x801A)
 *
 * Frame identifier (29 bits; the M-CAN document's terms): bits 0-6 the sender's node
 * number, bits 7-9 the command (Befehl), bits 10-25 an object handle (a device, the
 * directory, the central, a locomotive slot), bits 26-28 a priority. mcan_receive splits a
 * received frame into mcan_rx_node, mcan_rx_cmd, mcan_rx_handle and the data
 * (can_rx_frame_buf); mcan_send builds the identifier from a command word (bits 10-12
 * priority, bits 7-9 command, bits 0-6 node) and a handle.
 *
 * mcan_process runs every 4 ms (main loop slot 0). The master (is_master)
 * announces itself, takes the registration of slave MS1s - each sends its UID in steps,
 * the master answers - and runs its own operation. A slave registers with the master
 * (steps 0-7 under command 0x7E with its UID, its versions and its configuration), gets
 * its node number (mcan_node) and then exchanges locomotive slots and states.
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/mcan.h"
#include "../include/rom_tables.h"
#include "../include/ui.h"
#include "../include/lokstack.h"

extern u16  can_tx_enqueue_frame(u32 id, const u8 *data, u16 dlc);   /* build/asm/hal.s */
extern u16  can_rx_dequeue_frame(u8 *dest);                          /* build/asm/hal.s */
extern void mcan_master_request(void);                           /* 0x601A */
extern void mcan_object_request(void);                           /* 0x637A */
extern void mcan_send_status(void);                         /* 0x6A95 */
extern void mcan_receive_status(void);                             /* 0x6C73 */
extern void mcan_receive_control(void);                         /* 0x702B */
extern void mcan_receive_lok_state(void);                           /* 0x71F6 */
extern void mcan_lokstack_sync(void);                          /* 0x7642 */

/* this device's UID and versions, in the bootloader's flash metadata and the configuration
 * area: 0xFF10-0xFF13 UID, 0xFF14-0xFF1B, 0xFF24-0xFF27, 0xF700-0xF707, 0xF70C-0xF70F */
#define FLASH_W(a)  (*(const u16 *)(a))
#define FLASH_B(a)  (*(const u8 *)(a))

#define W(p, off)   (*(u16 *)((u8 *)(p) + (off)))

/* 0x5E70: the next received frame, split up; 0 if there is none */
u16 mcan_receive(void)
{
    if (can_rx_dequeue_frame(can_rx_frame_buf) == 0)
        return 0;
    mcan_rx_node = can_rx_frame_buf[3] & 0x7F;
    mcan_rx_cmd = (u8)(W(can_rx_frame_buf, 2) >> 7) & 7;
    mcan_rx_handle = (u16)((((u32)W(can_rx_frame_buf, 0) << 16) | W(can_rx_frame_buf, 2)) >> 10);
    return 1;
}

/* 0x5EA5: send a frame; 0 if the transmit ring is full. Nothing goes out (and 1 is
 * returned) for command 0 (cmd bits 0-6) before this device has a node number. */
u16 mcan_send(u16 cmd, u16 handle, const u8 *data, u16 dlc)
{
    u32 id;

    if (mcan_peer_node == 0 && (cmd & 0x7F) == 0)
        return 1;
    id = ((u32)handle << 10) | ((u32)(cmd & 0x1C00) << 16) | (cmd & 0x03FF);
    if (can_tx_enqueue_frame(id, data, dlc) == 0)
        return 0;
    return 1;
}

/* fill an 8-byte frame from four words */
static void words(u8 *buf, u16 a, u16 b, u16 c, u16 d)
{
    W(buf, 0) = a;
    W(buf, 2) = b;
    W(buf, 4) = c;
    W(buf, 6) = d;
}

/* the 8 bytes a registration step k (4-7) sends: UID and versions, configuration */
static void registration_data(u8 *buf, u8 k)
{
    switch (k) {
    case 4:
        words(buf, FLASH_W(0xFF10), FLASH_W(0xFF12), 2, 3);
        break;
    case 5:
        words(buf, FLASH_W(0xFF14), FLASH_W(0xFF16), FLASH_W(0xFF18), FLASH_W(0xFF1A));
        break;
    case 6:
        words(buf, FLASH_W(0xFF24), FLASH_W(0xFF26), FLASH_W(0xF70C), FLASH_W(0xF70E));
        break;
    case 7:
        words(buf, FLASH_W(0xF700), FLASH_W(0xF702), FLASH_W(0xF704), FLASH_W(0xF706));
        break;
    }
}

/* clear a slot and set it up for a locomotive the master announced (0x86F5, 0x8860) */
static void slot_from_master(s16 s)
{
    u8 *e = (u8 *)&lok_records[s];
    u8 *p;
    lok_state_t *disp;

    for (p = e; p < e + sizeof(lok_record_t); p++)
        *p = 0;
    lok_state_clear(s);
    lok_states[s].change_count = mcan_rx_dlc > 7 ? mcan_rx_data67 : 0;
    lok_records[s].kinds[0] = (u8)(mcan_rx_data01 >> 8);
    disp = &lok_states[s];
    disp->object = mcan_rx_data45;
    disp->request = 1;
}

/* master: a frame of a slave in registration, or of the bus */
static void master_frames(u8 *buf)
{
    u8 st, cmd, k, m;
    u16 t;

    (void)buf;
    while (mcan_receive()) {
        st = mcan_peer_node;
        cmd = mcan_rx_node;
        if (st != 0 && cmd == st) {
            mcan_peer_timeout = 0x4FF;
            if (mcan_rx_cmd == 0)
                mcan_receive_control();
            if (mcan_peer_handle == 0 || mcan_peer_handle != mcan_rx_handle) {
                if (mcan_rx_cmd == 1)
                    mcan_object_request();
                if (mcan_rx_handle == 1 && mcan_rx_cmd > 2 && mcan_rx_cmd <= 5)
                    mcan_master_request();
            }
            m = mcan_rx_cmd;
            if (mcan_rx_handle == 0 && m == 7 && mcan_rx_dlc > 3 && mcan_rx_data01 == 0 && mcan_rx_data23 == 0)
                mcan_link_flags |= 4;
            if (mcan_rx_handle == 2 && m == 2)
                mcan_receive_status();
            else if (m == 2)
                mcan_receive_lok_state();
            continue;
        }
        /* registration of a slave: step k brings UID byte k in handle bits 8-15 */
        if (cmd != 0x7E || mcan_rx_cmd != 0 || st != 0)
            continue;
        k = mcan_reg_step;
        if (k > 3)
            continue;
        t = (mcan_rx_handle >> 5) & 7;
        if (k != t)
            continue;
        if (k != 0 && (mcan_rx_handle & 0x1F) != mcan_reg_mid)
            continue;
        ((u8 *)&mcan_peer_uid_hi)[t] = (u8)(mcan_rx_handle >> 8);
        mcan_reg_step = k + 1;
        mcan_link_flags |= 1;
    }
}

/* slave: a frame for this node (command = its node number) */
static void slave_frame_for_me(void)
{
    u8 m, a0, a1, dlc;
    s16 s;
    u16 y;

    mcan_peer_timeout = 0x4FF;
    if (mcan_sd_handle != 0 &&
        (mcan_rx_handle == mcan_sd_handle || lok_slot_of_object(mcan_rx_handle) <= 9)) {
        m = mcan_rx_cmd;
        if (m == 0) {
            mcan_receive_control();
            m = mcan_rx_cmd;
        }
        if (m == 1)
            mcan_object_request();
    }
    y = mcan_rx_handle;
    if (y == mcan_peer_handle && mcan_rx_cmd == 7) {
        /* from the master: reset requests and identification */
        if (mcan_rx_data01 == 0 && mcan_rx_dlc == 8) {
            u16 a = mcan_rx_data67, b = mcan_rx_data45;
            if ((b == 0 && a == 0) || !(b == mcan_peer_uid_hi && a == mcan_peer_uid_lo)) {
                if (mcan_rx_data23 == mcan_peer_node || mcan_rx_data23 == 0) {
                    crt_restart();
                    goto ident;
                }
            }
            if (!(b == FLASH_W(0xFF10) && a == FLASH_W(0xFF12)) && mcan_node == mcan_rx_data23)
                crt_restart();
        }
    ident:
        if (mcan_rx_data01 == 0 && mcan_node == mcan_rx_data23) {
            if (mcan_rx_dlc == 4 ||
                (mcan_rx_dlc == 8 && mcan_rx_data45 == FLASH_W(0xFF10) && mcan_rx_data67 == FLASH_W(0xFF12)))
                mcan_link_flags |= 4;
        }
        y = mcan_rx_handle;
    }
    if (y == mcan_start_handle) {
        m = mcan_rx_cmd;
        if (m == 3) {
            /* the master's slot list */
            dlc = mcan_rx_dlc;
            if (dlc > 3) {
                a0 = (u8)(mcan_rx_data01 >> 8);
                if (a0 == 3 && (u8)mcan_rx_data01 == 0 && mcan_rx_data23 == 0 && dlc > 5 &&
                    mcan_rx_data45 != 0 &&
                    !(mcan_rx_data45 == mcan_sy_handle &&
                      (dlc <= 7 || mcan_rx_data67 == mcan_sy_change_count))) {
                    mcan_sy_handle = mcan_rx_data45;
                    if (mcan_rx_dlc > 7)
                        mcan_sy_change_count = mcan_rx_data67;
                    mcan_status_idx_overload = 0;
                    mcan_status_idx_mfx_reg = 0;
                    mcan_status_idx_mm_prog = 0;
                    mcan_status_idx_stop = 0;
                    mcan_list_index = 1;
                    mcan_link_flags |= 1;
                    if (mcan_reg_step > 9)
                        mcan_reg_step = 11;
                    a0 = (u8)(mcan_rx_data01 >> 8);
                }
                if ((u8)(a0 + 0x80) <= 2 && mcan_rx_data23 == mcan_list_index && (u8)mcan_rx_data01 == 2) {
                    if (dlc <= 5) {
                        mcan_reg_step++;
                        mcan_list_index = 0;
                    } else {
                        mcan_list_index = mcan_rx_data45;
                        if (lok_slot_of_object(mcan_rx_data45) > 9) {
                            s = lok_find_free_slot();
                            if (s <= 9)
                                slot_from_master(s);
                        }
                        if (lok_find_free_slot() > 9)
                            mcan_reg_step = 0x10;
                    }
                    mcan_link_flags |= 1;
                }
            }
            m = mcan_rx_cmd;
        }
        if (m == 4) {
            /* a slot of the master */
            dlc = mcan_rx_dlc;
            if (dlc > 7) {
                a1 = (u8)mcan_rx_data01;
                if (a1 == mcan_node) {
                    a0 = (u8)(mcan_rx_data01 >> 8);
                    if (a0 == 0x40 && mcan_rx_data45 != 0 && mcan_rx_data67 != 0 && mcan_reg_step == 9) {
                        mcan_reg_step = 10;
                        mcan_sd_handle = mcan_rx_data45;
                        mcan_sd_change_count = mcan_rx_data67;
                        system_status_flags &= ~0x10;
                        mcan_link_flags |= 1;
                    }
                } else {
                    a0 = (u8)(mcan_rx_data01 >> 8);
                }
                if ((u8)(a0 + 0x80) <= 2 && a1 == 0 && dlc > 5) {
                    /* a free slot when the locomotive has none, else its slot when the
                     * master's CAN address for it changed */
                    if (!(lok_slot_of_object(mcan_rx_data45) > 9 && (s = lok_find_free_slot()) <= 9)) {
                        s = lok_slot_of_object(mcan_rx_data45);
                        if (s > 9 || mcan_rx_dlc <= 7 || mcan_rx_data67 == lok_states[s].change_count)
                            s = -1;
                    }
                    if (s != -1) {
                        slot_from_master(s);
                        lok_states[s].controller = mcan_rx_data23;
                        if ((s8)lok_current_slot == s)
                            ui_enter_mode(1);
                    }
                }
            }
            m = mcan_rx_cmd;
        }
        if (m == 5 && mcan_rx_dlc > 5) {
            /* a slot the master frees */
            s = lok_slot_of_object(mcan_rx_data45);
            if (mcan_sy_handle != 0 && mcan_sy_handle == mcan_rx_data45)
                crt_restart();
            if (mcan_sd_handle != 0 && mcan_sd_handle == mcan_rx_data45)
                crt_restart();
            if (s <= 9) {
                lok_records[s].unused = 0xFF;
                if ((s8)lok_current_slot == s)
                    ui_enter_mode(1);
            }
        }
    }
    if (mcan_sy_handle != 0 && mcan_sy_handle == mcan_rx_handle && mcan_rx_cmd == 2)
        mcan_receive_status();
    else if (mcan_rx_cmd == 2)
        mcan_receive_lok_state();
}

/* slave: frames until there are none */
static void slave_frames(void)
{
    u8 cmd, d, k, b, dlc, f;

    while (mcan_receive()) {
        cmd = mcan_rx_node;
        if (cmd == mcan_peer_node && mcan_node != 0) {
            slave_frame_for_me();
            continue;
        }
        /* the master's answers to this device's registration */
        if (cmd != 0x7E || mcan_rx_cmd != 1 || (system_status_flags & 0x20) || mcan_node != 0)
            continue;
        d = (u8)mcan_rx_handle & 0x1F;
        k = mcan_reg_step;
        if (k <= 3) {
            if (((mcan_rx_handle >> 5) & 7) != k)
                goto restart;
            b = (u8)(mcan_rx_handle >> 8);
            if (FLASH_B(0xFF10 + k) != b)
                goto restart;
            if (k != 0) {
                if (d != mcan_reg_mid)
                    goto restart;
            } else {
                dlc = mcan_rx_dlc;
                if (dlc > 3) {
                    mcan_peer_uid_lo = mcan_rx_data23;
                    mcan_peer_uid_hi = mcan_rx_data01;
                } else {
                    mcan_peer_uid_lo = 0;
                    mcan_peer_uid_hi = 0;
                }
                mcan_peer_handle = dlc > 7 ? mcan_rx_data67 : 0;
            }
            mcan_reg_mid = d;
            mcan_reg_step = k + 1;
            mcan_link_flags |= 1;
            continue;
        }
        if ((mcan_rx_handle & 0xFFE0) != 0x00E0)
            continue;
        dlc = mcan_rx_dlc;
        if (dlc <= 6)
            continue;
        if (k > 8 || mcan_rx_data01 != FLASH_W(0xFF10) || mcan_rx_data23 != FLASH_W(0xFF12) ||
            d != mcan_reg_mid)
            goto restart;
        if (dlc > 7) {
            f = (u8)mcan_rx_data67;
            if (f & 1)
                enter_bootloader();
            else if (f & 2)
                system_status_flags |= 0x20;
        }
        mcan_node = (u8)(mcan_rx_data67 >> 8);
        if (mcan_node == 0)
            continue;
        if ((s8)mcan_node < 0) {
            mcan_node &= 0x7F;
            mcan_peer_node = mcan_node - 1;
        } else {
            mcan_peer_node = 0;
        }
        mcan_start_handle = mcan_rx_data45;
        if (mcan_start_handle == 0)
            mcan_start_handle = 1;
        mcan_reg_step = 9;
        mcan_link_flags |= 1;
        continue;
    restart:
        mcan_reg_mid = 0;
        mcan_reg_step = 0;
    }
}

/* 0x801A */
void mcan_process(void)
{
    u8 buf[12];
    u8 k;
    s16 n;
    u16 handle;

    if (is_master == 0)
        goto slave;

    /* ---- master ---- */
    if (!(mcan_link_flags & 0x80)) {
        words(buf, 0, 0, 0, 0);
        for (n = 7; n >= 0; n--)
            can_tx_enqueue_frame(0x380, buf, 8);
        mcan_link_flags |= 0x80;
    }
    master_frames(buf);
    if (mcan_peer_node == 0) {
        if (mcan_link_flags & 1) {
            k = mcan_reg_step;
            switch (k) {
            case 1:
                words(buf, FLASH_W(0xFF10), FLASH_W(0xFF12), 2, 0);
                break;
            case 2:
                words(buf, FLASH_W(0xFF14), FLASH_W(0xFF16), FLASH_W(0xFF18), FLASH_W(0xFF1A));
                break;
            case 3:
                words(buf, FLASH_W(0xFF24), FLASH_W(0xFF26), FLASH_W(0xF70C), FLASH_W(0xF70E));
                break;
            case 4:
                words(buf, FLASH_W(0xF700), FLASH_W(0xF702), FLASH_W(0xF704), FLASH_W(0xF706));
                break;
            case 8:
                words(buf, mcan_peer_uid_hi, mcan_peer_uid_lo, 1, 0x0100);
                break;
            }
            if (k == 8)
                handle = (u16)(k - 1) << 5;
            else
                handle = (u16)(((u8 *)&mcan_peer_uid_hi)[k - 1] * 8 + (k - 1)) << 5;
            handle += mcan_reg_mid;
            if (mcan_send(0x1CFE, handle, buf, 8)) {
                if (mcan_reg_step == 8)
                    mcan_peer_node = 1;
                if (mcan_reg_step == 4)
                    mcan_reg_step = 8;
                else
                    mcan_link_flags &= ~1;
                mcan_peer_timeout = 0x4FF;
            }
        } else if (mcan_reg_step != 0) {
            if (mcan_peer_timeout == 0) {
                mcan_reg_step = 0;
                mcan_reg_mid = (mcan_reg_mid + 1) & 0x1F;
            } else {
                mcan_peer_timeout--;
            }
        }
    } else {
        if (mcan_peer_timeout == 0) {
            mcan_slave_lost();
        } else {
            if ((mcan_peer_timeout & 0x3F) == 0)
                mcan_link_flags |= 2;
            mcan_peer_timeout--;
        }
        if (mcan_link_flags & 2) {
            words(buf, 0, 1, mcan_peer_uid_hi, mcan_peer_uid_lo);
            if (mcan_send(0x0F80, 0, buf, 8))
                mcan_link_flags &= ~2;
        }
        if (mcan_link_flags & 4) {
            words(buf, 0, 0, FLASH_W(0xFF10), FLASH_W(0xFF12));
            if (mcan_send(0x0F80, 0, buf, 8))
                mcan_link_flags &= ~4;
        }
        mcan_send_device_name();
    }
    mcan_send_status();
    mcan_lokstack_sync();
    return;

    /* ---- slave ---- */
slave:
    slave_frames();
    if (mcan_node == 0) {
        /* registration */
        if (system_status_flags & 0x20)
            return;
        if (mcan_link_flags & 1) {
            k = mcan_reg_step;
            if (k <= 3) {
                handle = (((u16)FLASH_B(0xFF10 + k) * 8 + k) << 5) + mcan_reg_mid;
                if (mcan_send(0x1C7E, handle, 0, 0)) {
                    mcan_link_flags &= ~1;
                    mcan_peer_timeout = 0x4FF;
                }
            } else if (k <= 7) {
                do {
                    registration_data(buf, k);
                    if (!mcan_send(0x1C7E, ((u16)k << 5) + mcan_reg_mid, buf, 8))
                        break;
                    k = ++mcan_reg_step;
                    if (k == 8)
                        mcan_link_flags &= ~1;
                    mcan_peer_timeout = 0x4FF;
                } while (k <= 7);
            }
        }
        if (mcan_peer_timeout == 0) {
            mcan_link_flags |= 1;
            mcan_reg_mid = 0;
            mcan_reg_step = 0;
        } else {
            mcan_peer_timeout--;
        }
        return;
    }

    /* registered */
    if (mcan_peer_timeout == 0) {
        crt_restart();
    } else {
        if ((mcan_peer_timeout & 0x3F) == 0)
            mcan_link_flags |= 2;
        mcan_peer_timeout--;
    }
    if (mcan_link_flags & 4) {
        words(buf, 0, mcan_node, FLASH_W(0xFF10), FLASH_W(0xFF12));
        if (mcan_send(0x0F80 | mcan_node, mcan_peer_handle, buf, 8))
            mcan_link_flags &= ~4;
    }
    if (mcan_link_flags & 2) {
        W(buf, 0) = 0;
        W(buf, 2) = mcan_peer_node;
        if (mcan_send(0x0F80 | mcan_node, mcan_peer_handle, buf, 4))
            mcan_link_flags &= ~2;
    }
    if (mcan_reg_step == 9 && ((mcan_link_flags & 1) || (ms_tick & 0x1FC) == 0)) {
        buf[0] = 0x40;
        buf[1] = mcan_node;
        if (mcan_send(0x1A00 | mcan_node, mcan_start_handle, buf, 2))
            mcan_link_flags &= ~1;
    }
    if ((mcan_link_flags & 1) || (ms_tick & 0x1FC) == 0) {
        k = mcan_reg_step;
        if ((u8)(k - 10) <= 5) {
            u16 r = 0;
            switch (k - 10) {
            case 0:
                buf[0] = 3;
                buf[1] = 0;
                r = mcan_send(0x1980 | mcan_node, mcan_start_handle, buf, 2);
                break;
            case 1:
                buf[0] = (u8)mcan_list_index;
                buf[1] = 1;
                r = mcan_send(0x1900 | mcan_node, mcan_sy_handle, buf, 2);
                break;
            case 2:
                buf[0] = (u8)mcan_list_index;
                buf[1] = 0;
                r = mcan_send(0x1900 | mcan_node, mcan_sy_handle, buf, 2);
                break;
            default:                            /* 13-15 */
                buf[8] = k == 14 ? 0x81 : k == 15 ? 0x82 : 0x80;
                buf[9] = 2;
                W(buf, 10) = mcan_list_index;
                r = mcan_send(0x1980 | mcan_node, mcan_start_handle, buf + 8, 4);
                break;
            }
            if (r)
                mcan_link_flags &= ~1;
        }
    }
    if (mcan_sd_handle == 0)
        return;
    if (mcan_sy_handle != 0)
        mcan_send_status();
    mcan_send_device_name();
    mcan_lokstack_sync();
}
