/*==========================================================================
 * signal/dcc.c - DCC locomotives on the track, in page $3C (include/page3c.h)
 *
 * lok_packets_next and lok_packets_refresh (signal/lok_packets.c) hand a DCC slot
 * (LOK_DCC) to these. Each call writes one packet as a command entry of type 6
 * (CMD_DCC, track_cmd.h), which signal/signal_gen.c sends: a preamble of DCC_PREAMBLE ones,
 * each byte after a 0, the XOR byte, the end bit.
 *
 * Packets (RCN-211, RCN-212, RCN-214): the address, 1 byte for 1-127 or 2 (0xC0 | high, low)
 * above 127 or with LOKP_DCC_LONG; speed in 128 steps (0x3F, DSSSSSSS: 0 stop, 2-127 steps
 * 1-126), 28 (01DCSSSS, C the low bit, codes 4-31) or 14 (01DFSSSS, F the light, codes
 * 2-15) by LOKP_DCC_STEPS, from the slot's 0-127 and bit 7 reverse; the function groups
 * F0-F4 (100FFFFF, F0 in bit 4), F5-F8 (1011FFFF), F9-F12 (1010FFFF), F13-F20 (0xDE); CV
 * writes on the main (POM: 1110 11VV VVVVVVVV DDDDDDDD, CV = V + 1), sent twice: a decoder
 * acts on the second of two identical packets. When the entry before is for the same decoder,
 * an idle packet comes first, so that two packets to one decoder are at least 5 ms apart; the
 * job or the refresh phase then goes on at the next call.
 *
 * Jobs (dcc_slot_job, pkt_steps the packets left of pkt_kind, a CV write's in dcc_cv,
 * dcc_cv_value, dcc_cv_address), from the slot state's
 * track_pending: the CV writes first (VMAX CV5, ACC CV3, DEC CV4, REG prog_cv, RESET CV8 = 8
 * to the old address, which then becomes default_address as for MM); then speed and
 * direction (twice after a direction change); then the groups of the changed functions
 * (track_functions). An address change (bit 2, set after PROG LOK?) programs the decoder in
 * service mode (signal/dcc_service.c); the name (bit 3) changes only the entry.
 * The refresh (dcc_slot_refresh) sends a slot's speed and its used function groups in turn,
 * one packet per call, mm_phase the next.
 *
 * Functions:
 *   dcc_slot_job      - the next packet of a DCC slot's pending work
 *   dcc_slot_refresh  - the slot's next periodic packet
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/lokstack.h"
#include "../include/page3c.h"
#include "../include/track_cmd.h"
#include "../include/dcc.h"

#define CMD_ENTRY_SIZE  12

/* pkt_kind of a DCC job */
#define JOB_SPEED       0
#define JOB_GROUP       1               /* + the function group 0-3 */
#define JOB_CV          8               /* a CV write: dcc_cv, dcc_cv_value */

/* track_pending bits */
#define PEND_SPEED      0x0001
#define PEND_DIRECTION  0x0002
#define PEND_ADDRESS    0x0004
#define PEND_NAME       0x0008
#define PEND_VMAX       0x0010
#define PEND_ACCEL      0x0020
#define PEND_DECEL      0x0040
#define PEND_VOLUME     0x0080
#define PEND_CV         0x0100
#define PEND_RESET      0x4000
#define PEND_DCC        (PEND_SPEED | PEND_DIRECTION | PEND_ADDRESS | PEND_NAME | PEND_VMAX | PEND_ACCEL \
                         | PEND_DECEL | PEND_VOLUME | PEND_CV | PEND_RESET)

/* the function group 0-3 of function k (F0-F4, F5-F8, F9-F12, F13-F15) */
static u8 group_of(u8 k)
{
    return k <= 4 ? 0 : k <= 8 ? 1 : k <= 12 ? 2 : 3;
}

/* the address bytes of e into p; their count */
static u8 put_address(const lok_record_t *e, u16 address, u8 *p)
{
    if (address > 127 || (e->protocol_flags & LOKP_DCC_LONG)) {
        p[0] = 0xC0 | (u8)(address >> 8);
        p[1] = (u8)address;
        return 2;
    }
    p[0] = (u8)address;
    return 1;
}

/* the entry of a packet: a preamble of preamble ones, n bytes, the XOR byte added (also
 * signal/dcc_service.c) */
void dcc_write_entry(const u8 *bytes, u8 n, u8 preamble)
{
    u8 *e = (u8 *)track_cmd_write_ptr;
    u8 x = 0, i;

    e[0] = CMD_DCC;
    e[1] = n + 2;                       /* steps: the preamble, each byte, the XOR byte */
    e[2] = preamble;
    e[3] = n + 1;
    for (i = 0; i < n; i++) {
        e[4 + i] = bytes[i];
        x ^= bytes[i];
    }
    e[4 + n] = x;
    track_cmd_advance();
}

/* a packet for the decoder whose address takes the first na bytes, but an idle packet
 * instead when the entry before is for the same decoder: 0 then, 1 when it wrote the packet */
static u8 write_packet(const u8 *bytes, u8 n, u8 na)
{
    static const u8 idle[2] = { 0xFF, 0x00 };
    u8 *prev = (u8 *)track_cmd_write_ptr - CMD_ENTRY_SIZE;

    if (prev < track_cmd_buf)
        prev += sizeof track_cmd_buf;
    if (prev[0] == CMD_DCC && prev[4] == bytes[0] && (na == 1 || prev[5] == bytes[1])) {
        dcc_write_entry(idle, 2, DCC_PREAMBLE);
        return 0;
    }
    dcc_write_entry(bytes, n, DCC_PREAMBLE);
    return 1;
}

/* the speed instruction into p; its byte count */
static u8 put_speed(const lok_record_t *e, u8 speed, u16 functions, u8 *p)
{
    u8 v = speed & 0x7F, fwd = (speed & 0x80) ? 0 : 1, s;

    switch (e->protocol_flags & LOKP_DCC_STEPS) {
    case LOKP_DCC_28:
        s = v ? (u8)(((u16)v * 28 + 126) / 127) + 3 : 0;
        p[0] = 0x40 | fwd << 5 | (s & 1) << 4 | s >> 1;
        return 1;
    case LOKP_DCC_14:
        s = v ? (u8)(((u16)v * 14 + 126) / 127) + 1 : 0;
        p[0] = 0x40 | fwd << 5 | (u8)(functions & 1) << 4 | s;
        return 1;
    default:
        p[0] = 0x3F;
        p[1] = fwd << 7 | (v == 0 ? 0 : v >= 126 ? 127 : v + 1);
        return 2;
    }
}

/* the function group's instruction into p; its byte count */
static u8 put_group(u8 group, u16 f, u8 *p)
{
    switch (group) {
    case 0:
        p[0] = 0x80 | (u8)(f & 1) << 4 | (u8)(f >> 1 & 0x0F);
        return 1;
    case 1:
        p[0] = 0xB0 | (u8)(f >> 5 & 0x0F);
        return 1;
    case 2:
        p[0] = 0xA0 | (u8)(f >> 9 & 0x0F);
        return 1;
    default:
        p[0] = 0xDE;
        p[1] = (u8)(f >> 13);
        return 2;
    }
}

/* the packet of job kind for slot, or the idle packet before it: write_packet */
static u8 job_packet(u16 slot, u8 kind)
{
    lok_record_t *e = &lok_records[slot];
    lok_state_t *d = &lok_states[slot];
    u8 p[6], na, n;

    na = put_address(e, kind == JOB_CV ? dcc_cv_address : e->address, p);
    if (kind == JOB_SPEED) {
        n = na + put_speed(e, d->speed, d->functions, p + na);
    } else if (kind == JOB_CV) {
        p[na] = 0xEC | (u8)((dcc_cv - 1) >> 8 & 3);
        p[na + 1] = (u8)(dcc_cv - 1);
        p[na + 2] = dcc_cv_value;
        n = na + 3;
    } else {
        n = na + put_group(kind - JOB_GROUP, d->functions, p + na);
    }
    return write_packet(p, n, na);
}

/* a CV write job: two identical packets */
static void start_cv(u16 cv, u8 value, u16 address)
{
    dcc_cv = cv;
    dcc_cv_value = value;
    dcc_cv_address = address;
    pkt_kind = JOB_CV;
    pkt_steps = 2;
}

u16 dcc_slot_job(u16 slot)
{
    lok_record_t *e = &lok_records[slot];
    lok_state_t *d = &lok_states[slot];
    u16 x, f;
    u8 k;

    if (pkt_steps == 0) {
        x = d->track_pending;
        d->track_pending = x & ~(PEND_ADDRESS | PEND_NAME | PEND_VOLUME);
        if (x & PEND_ADDRESS) {
            /* the new address into the decoder: service mode, after PROG LOK? */
            dcc_service_start(SVC_ADDRESS, e->address,
                              e->address > 127 || (e->protocol_flags & LOKP_DCC_LONG));
            dcc_svc.auto_end = 1;
            return dcc_service_next();
        }
        if (x & PEND_VMAX) {
            d->track_pending &= ~PEND_VMAX;
            start_cv(5, e->vmax, e->address);
        } else if (x & PEND_ACCEL) {
            d->track_pending &= ~PEND_ACCEL;
            start_cv(3, e->accel, e->address);
        } else if (x & PEND_DECEL) {
            d->track_pending &= ~PEND_DECEL;
            start_cv(4, e->decel, e->address);
        } else if (x & PEND_CV) {
            d->track_pending &= ~PEND_CV;
            start_cv(d->prog_cv, d->prog_value, e->address);
            d->prog_cv = 0;
        } else if (x & PEND_RESET) {
            /* the decoder's own reset; the entry takes the address it has after it */
            d->track_pending &= ~PEND_RESET;
            start_cv(8, 8, e->address);
            LO8(d->can_pending) |= 0xF4;
            LO8(d->save_pending) |= 0xF4;
            e->address = e->default_address;
            e->vmax = 0xFF;
            e->accel = 4;
            e->decel = 3;
            e->volume = 0xFF;
            lok_check_conflict(slot);
            lok_recheck_conflicts();
        } else if (x & (PEND_SPEED | PEND_DIRECTION)) {
            d->track_pending &= ~(PEND_SPEED | PEND_DIRECTION);
            pkt_kind = JOB_SPEED;
            pkt_steps = (x & PEND_DIRECTION) ? 2 : 1;
        } else if ((f = d->track_functions) != 0) {
            for (k = 0; !((f >> k) & 1); k++)
                ;
            pkt_kind = JOB_GROUP + group_of(k);
            pkt_steps = 1;
            for (k = 0; k < 16; k++)
                if (group_of(k) == pkt_kind - JOB_GROUP)
                    f &= ~((u16)1 << k);
            d->track_functions = f;
        } else {
            return 0;
        }
    }
    if (job_packet(slot, pkt_kind))
        pkt_steps--;
    return 1;
}

/* the groups slot uses: a function with a key, or one that is on */
static u8 groups_used(u16 slot)
{
    const lok_function_t *f = lok_records[slot].functions;
    u16 on = lok_states[slot].functions;
    u8 used = 0, k;

    for (k = 0; k < 16; k++)
        if (f[k].icon != 0 || ((on >> k) & 1))
            used |= 1 << group_of(k);
    return used;
}

u16 dcc_slot_refresh(u16 slot)
{
    lok_state_t *d = &lok_states[slot];
    u8 used, ph;

    used = groups_used(slot);
    ph = d->mm_phase;
    while (ph != 0 && !((used >> (ph - 1)) & 1))
        ph = ph < 4 ? ph + 1 : 0;
    if (job_packet(slot, ph == 0 ? JOB_SPEED : JOB_GROUP + ph - 1))
        d->mm_phase = ph < 4 ? ph + 1 : 0;
    return 1;
}
