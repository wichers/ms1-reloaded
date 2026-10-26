/*==========================================================================
 * signal/lok_packets.c - the locomotives' track packets after a change (0x40A3)
 *
 * lok_packets_next writes the next packet a locomotive slot needs into the command
 * buffer and returns 1, or returns 0 when no slot needs one. pkt_slot is the slot in
 * service, pkt_steps the packets left of its job, pkt_kind the job. A slot's pending jobs
 * are bits of its state's track_pending; its changed functions are bits of
 * track_functions.
 *
 * MM slots (LOK_MM), pkt_kind:
 *   0-8   one packet: speed (0) or function 1-8 (mm_write_packet)
 *   0x80  direction change: 2 speed packets, 4 MM1 reverse packets (LOKP_MM1_REVERSE),
 *         4 MM2 direction packets (LOKP_MM2)
 *   0x90+ decoder programming, 250 steps: speed 0 and reverse packets on address 0 (index
 *         80), then the item's index and its value, each sent as the address of reverse
 *         packets (mm_prog_addr); system_status_flags bit 3 meanwhile
 * mfx slots (LOK_MFX), pkt_kind:
 *   0     speed/direction and functions; 4 direction frames first on a direction change
 *   1-5   writing a configuration value (3 frames and a pause)
 *   10+   the name, one character per 4 steps
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/lokstack.h"
#include "../include/rom_tables.h"
#include "../include/str.h"
#include "../include/track_cmd.h"

/* the value the mfx CV 1 write sends */
static const u8 mfx_cv1_value[] = { 0xC6 };


u16 lok_packets_next(void)
{
    lok_record_t *e;
    lok_state_t *d;
    u8 slot, start, n, k, bit, st;
    u16 x, w;

    if (pkt_steps == 0 && (system_status_flags & 1)) {
        track_cmd_pause(2000);
        return 1;
    }
    slot = pkt_slot;
    start = slot ? slot : 10;

scan:
    if (slot > 9) {
        slot = 0;
        pkt_slot = 0;
    }
    e = &lok_records[slot];
    d = &lok_states[slot];
    if (e->unused == 0) {
        if (e->flags == 0 && !(d->flags & 0x40))
            goto service;
        if ((d->track_pending & 0x41FC) && (u8)(e->protocol - 2) <= 2)
            goto service;
        if (system_status_flags & 8)
            goto service;
    }
    if (e->unused != 0 && (d->track_pending & 0x1000)) {
        d->track_pending &= ~0x1000;
        if (e->protocol == LOK_MFX) {
            mfx_frame_ping(e->address, 0, 0);
            return 1;
        }
        st = system_status_flags;
        if (st & 8) {
            system_status_flags = st & ~8;
            mcan_status_pending |= 8;
        }
        goto check;
    }
    pkt_slot = ++slot;
    pkt_steps = 0;
check:
    if (slot != start)
        goto scan;
    return 0;

next_slot:
    slot = pkt_slot + 1;
    pkt_slot = slot;
    goto check;

service:
    if (e->protocol == LOK_MFX)
        goto mfx;
    if (e->protocol != LOK_MM) {
        pkt_slot = ++slot;
        goto check;
    }

    /* MM: start a job */
    if (pkt_steps == 0) {
        x = d->track_pending;
        if ((x & 0x41FC) == 0) {
            if (x & 2) {
                pkt_steps = 10;
                pkt_kind = 0x80;
                d->track_pending = x & ~2;
                w = d->track_functions;
            } else if (x & 1) {
                pkt_steps = 2;
                pkt_kind = 0;
                d->track_pending = x & ~1;
                w = d->track_functions;
            } else {
                /* the highest changed function the decoder takes */
                w = d->track_functions;
                while (w & 0x1FF) {
                    bit = 8;
                    pkt_kind = 8;
                    if (!(w & 0x100)) {
                        do {
                            pkt_kind = --bit;
                        } while (!((1 << bit) & w));
                    }
                    w &= ~(1 << bit);
                    d->track_functions = w;
                    if (!(bit > 4 && !(e->protocol_flags & LOKP_F5_F8)) && (bit == 0 || (e->protocol_flags & LOKP_MM2))) {
                        pkt_steps = 2;
                        break;
                    }
                    w = d->track_functions;
                }
            }
        } else if (!(e->protocol_flags & LOKP_PROGRAMMABLE)) {
            d->track_pending = x & 0xBE03;
            w = d->track_functions;
        } else {
            pkt_kind = 0;
            if (x & 4) {
                pkt_kind = 0x90;
                x &= ~4;
            } else if (x & 0x10) {
                pkt_kind = 0x91;
                x &= ~0x10;
            } else if (x & 0x20) {
                pkt_kind = 0x92;
                x &= ~0x20;
            } else if (x & 0x40) {
                pkt_kind = 0x93;
                x &= ~0x40;
            } else if (x & 0x80) {
                pkt_kind = 0x94;
                x &= ~0x80;
            } else if (x & 0x100) {
                pkt_kind = 0x95;
                x &= ~0x100;
            } else if (!(x & 0x4000)) {
                x &= 0xBE03;
            } else {
                /* the new address becomes the slot's, with default parameters */
                pkt_kind = 0x96;
                d->track_pending = x & ~0x4000;
                LO8(d->can_pending) |= 0xF4;
                LO8(d->save_pending) |= 0xF4;
                e->address = e->default_address;
                e->vmax = 0xFF;
                e->accel = 4;
                e->decel = 3;
                e->volume = 0xFF;
                lok_check_conflict(slot);
                lok_recheck_conflicts();
                lok_recheck_conflicts();
                goto mm_started;
            }
            d->track_pending = x;
mm_started:
            if (pkt_kind != 0)
                pkt_steps = 250;
            w = d->track_functions;
        }
        d->track_functions = w & ~1;
    }
    if (pkt_steps == 0)
        goto next_slot;

    /* MM: the job's next packet */
    n = --pkt_steps;
    k = pkt_kind;
    if (!(k & 0x80)) {
        mm_write_packet(e->address, d->speed, d->functions, k);
        return 1;
    }
    if ((u8)(k + 0x70) <= 15) {
        if (n == 0) {
            system_status_flags &= ~8;
            mcan_status_pending |= 8;
        } else {
            st = system_status_flags;
            if (!(st & 8)) {
                system_status_flags = st | 8;
                mcan_status_pending |= 8;
            }
        }
        switch (n / 10) {
        case 5:
        case 11:
            mm_write_packet(80, 0, 0, 0);
            return 1;
        case 6:
        case 12: case 13: case 14: case 15: case 16: case 17: case 18:
            mm_write_reverse(80, 0);
            return 1;
        case 7:
        case 9:
            mm_write_packet(mm_prog_addr, 0, 0, 0);
            return 1;
        case 8:
            /* the item's value */
            switch (k - 0x90) {
            case 0: mm_prog_addr = LO8(e->address); break;
            case 1:
                mm_prog_addr = e->vmax >> 2;
                if (mm_prog_addr <= 1)
                    mm_prog_addr = 2;
                break;
            case 2: mm_prog_addr = e->accel < 0x3F ? e->accel : 0x3F; break;
            case 3: mm_prog_addr = e->decel < 0x3F ? e->decel : 0x3F; break;
            case 4: mm_prog_addr = (u8)((s16)((u16)e->volume << 6) / 255); break;
            case 5:
                mm_prog_addr = d->prog_value;
                if (n % 10 == 0) {
                    LO8(d->prog_cv) = 0;
                    HI8(d->prog_cv) = 0;
                }
                break;
            case 6: mm_prog_addr = 8; break;
            }
            break;
        case 10:
            /* the item's index */
            switch (k - 0x90) {
            case 0: mm_prog_addr = 1; break;
            case 1: mm_prog_addr = 5; break;
            case 2: mm_prog_addr = 3; break;
            case 3: mm_prog_addr = 4; break;
            case 4: mm_prog_addr = 0x3F; break;
            case 5: mm_prog_addr = LO8(d->prog_cv); break;
            case 6: mm_prog_addr = 8; break;
            }
            break;
        default:
            track_cmd_pause(20000);
            return 1;
        }
        mm_prog_addr %= 80;
        if (mm_prog_addr == 0)
            mm_prog_addr = 80;
        mm_write_reverse(mm_prog_addr, 0);
        return 1;
    }
    /* direction change */
    switch (n) {
    case 0: case 1: case 2: case 3:
        if (e->protocol_flags & LOKP_MM2)
            mm_write_direction(e->address, d->speed ^ ((d->track_pending & 2) ? 0x80 : 0), d->functions);
        break;
    case 4: case 5: case 6: case 7:
        if (e->protocol_flags & LOKP_MM1_REVERSE)
            mm_write_reverse(e->address, LO8(d->functions));
        break;
    case 8: case 9:
        mm_write_packet(e->address, (d->speed & 0x80) ^ ((d->track_pending & 2) ? 0 : 0x80),
                                d->functions, 0);
        break;
    }
    return 1;

mfx:
    /* mfx: start a job */
    if (pkt_steps == 0) {
        pkt_steps = 4;
        x = d->track_pending;
        if (x & 0x10) {
            if (e->mfx_vmax_cv != 0)
                pkt_kind = 1;
            else
                pkt_steps = 0;
            x &= ~0x10;
        } else if (x & 0x20) {
            if (e->mfx_accel_cv != 0)
                pkt_kind = 2;
            else
                pkt_steps = 0;
            x &= ~0x20;
        } else if (x & 0x40) {
            if (e->mfx_accel_cv != 0)
                pkt_kind = 3;
            else
                pkt_steps = 0;
            x &= ~0x40;
        } else if (x & 0x80) {
            if (e->mfx_volume_cv != 0)
                pkt_kind = 4;
            else
                pkt_steps = 0;
            x &= ~0x80;
        } else if (x & 0x4000) {
            pkt_kind = 5;
            x &= ~0x4000;
        } else if (x & 8) {
            if (e->mfx_name_cv != 0)
                pkt_kind = 10;
            else
                pkt_steps = 0;
            x &= ~8;
        } else {
            if ((x & 3) || d->track_functions != 0) {
                pkt_kind = 0;
                pkt_steps = (x & 2) ? 5 : 1;
            } else {
                pkt_steps = 0;
            }
            goto mfx_run;
        }
        d->track_pending = x;
    }
mfx_run:
    n = pkt_steps;
    if (n == 0)
        goto next_slot;
    k = pkt_kind;
    switch (k) {
    case 0:
        mfx_frame_begin(e->address);
        if (pkt_steps > 1) {
            mfx_put_estop(d->speed, e->mfx_drive_cmd);
            LO8(d->track_pending) &= ~2;
        } else {
            if (d->track_pending & 1)
                mfx_put_drive(d->speed, e->mfx_drive_cmd);
            if (d->track_functions != 0)
                mfx_put_functions(d->functions, e->mfx_function_cmd);
            LO8(d->track_pending) &= ~1;
            d->track_functions = 0;
        }
        mfx_frame_end();
        break;
    case 1:
        if (e->vmax == 0)
            e->vmax = 1;
        if (n <= 1)
            goto pause;
        mfx_frame_write_cv(e->address, e->mfx_vmax_cv + 2, 1, &e->vmax);
        break;
    case 2:
        if (n <= 1)
            goto pause;
        mfx_frame_write_cv(e->address, e->mfx_accel_cv + 1, 1, &e->accel);
        break;
    case 3:
        if (n <= 1)
            goto pause;
        mfx_frame_write_cv(e->address, e->mfx_accel_cv + 2, 1, &e->decel);
        break;
    case 4:
        if (n <= 1)
            goto pause;
        mfx_frame_write_cv(e->address, e->mfx_volume_cv + 1, 1, &e->volume);
        break;
    case 5:
        if (n <= 1)
            goto pause;
        mfx_frame_write_cv(e->address, 1, 1, mfx_cv1_value);
        break;
    default:
        /* the name, character k - 10 */
        if ((s16)str_len(e->name) < (s16)(k - 10)) {
            pkt_steps = 1;
        } else if (pkt_steps > 1) {
            mfx_frame_write_cv(e->address, e->mfx_name_cv + k - 10, 1, &e->name[k - 10]);
        } else {
            mfx_cmd_syncs(e->mfx_write_syncs);
            pkt_kind++;
            pkt_steps = 5;
        }
        break;
    }
    goto mfx_done;
pause:
    mfx_cmd_syncs(e->mfx_write_syncs);
mfx_done:
    if (--pkt_steps == 0)
        pkt_slot++;
    return 1;
}

/* The periodic packets: per call one packet of the next slot, refresh_slot. An MM slot
 * cycles through 16 phases (mm_phase): speed in phases 1, 5, 9, 13, F1-F4 in 2, 6, 10, 14
 * (LOKP_MM2), F5-F8 in 3, 7, 11, 15 (LOKP_MM2 and LOKP_F5_F8), nothing
 * in 0, 4, 8, 12, and moves on to the next slot every 4 phases; an mfx slot sends one
 * speed and functions frame. Returns 0 (and starts over) after the last slot. */
u16 lok_packets_refresh(void)
{
    lok_record_t *e;
    lok_state_t *d;
    u8 s, ph;

    for (s = refresh_slot;; ) {
        if (s > 9) {
            refresh_slot = 0;
            return 0;
        }
        e = &lok_records[s];
        d = &lok_states[s];
        if (e->unused != 0 || (d->flags & 0xC0) || e->flags != 0) {
            refresh_slot = ++s;
            track_scheduler_hold = 0;
            continue;
        }
        if (e->protocol == LOK_MFX) {
            mfx_frame_begin(e->address);
            mfx_put_drive(d->speed, e->mfx_drive_cmd);
            mfx_put_functions(d->functions, e->mfx_function_cmd);
            mfx_frame_end();
            refresh_slot++;
            return 1;
        }
        if (e->protocol != LOK_MM) {
            refresh_slot = ++s;
            continue;
        }
        ph = d->mm_phase;
        if (ph <= 15) {
            switch (ph & 3) {
            case 1:
                mm_write_packet(e->address, d->speed, d->functions, 0);
                break;
            case 2:
                if (e->protocol_flags & LOKP_MM2)
                    mm_write_packet(e->address, d->speed, d->functions, (ph + 2) >> 2);
                break;
            case 3:
                if ((e->protocol_flags & (LOKP_MM2 | LOKP_F5_F8)) == (LOKP_MM2 | LOKP_F5_F8))
                    mm_write_packet(e->address, d->speed, d->functions, (ph + 0x11) >> 2);
                break;
            }
        }
        ph = d->mm_phase;
        d->mm_phase = ph + 1;
        if ((u8)(ph + 1) > 15)
            d->mm_phase = 0;
        if ((d->mm_phase & 3) == 0)
            refresh_slot++;
        return 1;
    }
}
