/*==========================================================================
 * signal/dcc_service.c - DCC service mode: reading and writing a decoder's CVs on the track,
 * in page $3C (include/page3c.h)
 *
 * Service mode (RCN-216, direct mode) acts on every DCC decoder on the track, so the user
 * confirms that only the one to program is on it ("PROG LOK?"). While it runs (dcc_svc.state),
 * track_scheduler gives the track to dcc_service_next alone: no locomotive packets, no mfx.
 * Packets have a SVC_PREAMBLE-bit preamble. A job is a list of operations; per operation:
 *   PH_PRE    3 reset packets (the first operation: 20, the decoders' power-on)
 *   PH_INSTR  5 instruction packets: 0111CCVV VVVVVVVV DDDDDDDD, CC 01 verify the byte, 11
 *             write it, 10 a bit (D = 111KDBBB, K = 1 write); dcc_svc.ack cleared at the first
 *   PH_POST   reset packets until ACK_WAIT_MS after the last instruction was written: the
 *             command buffer runs that far ahead of the track at most, and the decoder's
 *             acknowledgement (6 ms) has come by then
 * The acknowledgement: a decoder that matches a verify, or has written, draws 60 mA or more
 * for 6 ms. dcc_ack_sample, every main-loop pass, sets dcc_svc.ack when the current stays
 * ACK_MIN_MA above its running baseline for ACK_SAMPLES passes: version 4011 from
 * track_current; version 4010, which measures no current, from its over-current comparator
 * (PB6 low) with the threshold lowered to about 60 mA (PWM channels 2-3, as signal_gen_init
 * sets it from a limit) while service mode is on. More than LIMIT_MA for LIMIT_MS ends service
 * mode with STOP.
 *
 * Jobs (dcc_service_start): a CV read (8 bit verifies, then the byte verified), a CV write, an
 * address (CV1 and CV29 bit 5 clear, or CV17/CV18 and bit 5 set). The job's status: every
 * packet acknowledged, or not (a decoder without acknowledgement, or 4010 hardware whose
 * comparator does not see it, still takes the writes).
 *
 * Functions:
 *   dcc_service_start   - begin a job, entering service mode
 *   dcc_service_status  - the job's status and a read's value
 *   dcc_service_end     - leave service mode
 *   dcc_service_next    - the next packet (track_scheduler)
 *   dcc_ack_sample      - the acknowledgement detector (the main loop, every pass)
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/page3c.h"
#include "../include/dcc.h"
#include "../include/rom_tables.h"

#define POWER_ON_RESETS 20
#define PRE_RESETS      3
#define INSTR_PACKETS   5
#define ACK_WAIT_MS     60
#define ACK_MIN_MA      50
#define ACK_SAMPLES     4
#define LIMIT_MA        500
#define LIMIT_MS        100
/* version 4010: the comparator threshold for about 60 mA, limit L as signal_gen_init uses it:
 * L (L + 1000) / 1550 */
#define SVC_PWM_DUTY    41

#define PH_PRE          0
#define PH_INSTR        1
#define PH_POST         2
#define PH_IDLE         3

#define CC_VERIFY       1
#define CC_BIT          2
#define CC_WRITE        3

static void reset_packet(void)
{
    static const u8 reset[2] = { 0x00, 0x00 };

    dcc_write_entry(reset, 2, SVC_PREAMBLE);
}

static void instruction_packet(void)
{
    u8 p[3], k = dcc_svc.op;
    u16 cv = dcc_svc.cv[k] - 1;

    p[0] = 0x70 | dcc_svc.kind[k] << 2 | (u8)(cv >> 8 & 3);
    p[1] = (u8)cv;
    p[2] = dcc_svc.data[k];
    dcc_write_entry(p, 3, SVC_PREAMBLE);
}

static void add_op(u8 kind, u16 cv, u8 data)
{
    u8 k = dcc_svc.nops++;

    dcc_svc.kind[k] = kind;
    dcc_svc.cv[k] = cv;
    dcc_svc.data[k] = data;
}

u16 dcc_service_end(void)
{
    if (dcc_svc.state == 0)
        return 0;
    dcc_svc.state = 0;
    system_status_flags &= ~8;
    mcan_status_pending |= 8;
    if (message_id == 5)                    /* PROG, which no message replaces: ends here */
        system_status_flags &= ~0x80;
    if (dcc_svc.stop) {
        system_status_flags |= 1;
        mcan_status_pending |= 1;
    }
#if MS1_RELEASE == 0x4010
    PWMDTY23 = (u16)((u32)flash_current_limit * (u16)(flash_current_limit + 1000) / 1550);
#endif
    return 1;
}

static void job_done(void)
{
    u16 all = (u16)((1UL << dcc_svc.nops) - 1);

    if (dcc_svc.job == SVC_READ)
        dcc_svc.status = (dcc_svc.acks >> 8) & 1 ? SVC_OK : SVC_NO_ACK;
    else
        dcc_svc.status = (dcc_svc.acks & all) == all ? SVC_OK : SVC_NO_ACK;
    dcc_svc.phase = PH_IDLE;
    if (dcc_svc.auto_end) {
        dcc_service_end();
        if (dcc_svc.status != SVC_OK)
            show_message(LANG_ERROR - 0x16, 0x3E);
    }
}

/* an operation's result; the next one, or the job's end */
static void op_done(void)
{
    u8 k = dcc_svc.op;

    if (dcc_svc.ack) {
        dcc_svc.acks |= (u16)1 << k;
        if (dcc_svc.job == SVC_READ && k < 8)
            dcc_svc.value |= 1 << k;
    }
    if (dcc_svc.job == SVC_READ && k == 7)
        dcc_svc.data[8] = dcc_svc.value;
    if (++dcc_svc.op == dcc_svc.nops) {
        job_done();
        return;
    }
    dcc_svc.phase = PH_PRE;
    dcc_svc.count = PRE_RESETS;
}

u16 dcc_service_start(u16 job, u16 cv, u16 value)
{
    u8 k;

    if (dcc_svc.state == 0) {
        dcc_svc.state = 1;
        dcc_svc.stop = system_status_flags & 1;
        system_status_flags = (system_status_flags & ~1) | 8;
        mcan_status_pending |= 9;
#if MS1_RELEASE == 0x4010
        PWMDTY23 = SVC_PWM_DUTY;
        dcc_svc.baseline = 0;
#else
        dcc_svc.baseline = track_current << 4;
#endif
        dcc_svc.count = POWER_ON_RESETS;
    } else {
        dcc_svc.count = PRE_RESETS;
    }
    dcc_svc.phase = PH_PRE;
    dcc_svc.job = (u8)job;
    dcc_svc.op = dcc_svc.nops = 0;
    dcc_svc.acks = 0;
    dcc_svc.value = 0;
    dcc_svc.status = SVC_BUSY;
    dcc_svc.auto_end = 0;
    switch (job) {
    case SVC_READ:
        for (k = 0; k < 8; k++)
            add_op(CC_BIT, cv, 0xE8 | k);           /* verify: bit k is 1 */
        add_op(CC_VERIFY, cv, 0);                   /* the byte, from the bits */
        break;
    case SVC_WRITE:
        add_op(CC_WRITE, cv, (u8)value);
        break;
    case SVC_ADDRESS:
        if (value) {
            add_op(CC_WRITE, 17, 0xC0 | (u8)(cv >> 8));
            add_op(CC_WRITE, 18, (u8)cv);
            add_op(CC_BIT, 29, 0xF8 | 5);           /* write: bit 5 is 1 */
        } else {
            add_op(CC_WRITE, 1, (u8)cv);
            add_op(CC_BIT, 29, 0xF0 | 5);           /* write: bit 5 is 0 */
        }
        break;
    }
    return 1;
}

u16 dcc_service_status(void)
{
    return (u16)dcc_svc.value << 8 | dcc_svc.status;
}

u16 dcc_service_next(void)
{
    switch (dcc_svc.phase) {
    case PH_PRE:
        reset_packet();
        if (--dcc_svc.count == 0) {
            dcc_svc.phase = PH_INSTR;
            dcc_svc.count = INSTR_PACKETS;
            dcc_svc.ack = 0;
            dcc_svc.above = 0;
        }
        return 1;
    case PH_INSTR:
        instruction_packet();
        if (--dcc_svc.count == 0) {
            dcc_svc.phase = PH_POST;
            dcc_svc.t_last = ms_tick;
        }
        return 1;
    case PH_POST:
        reset_packet();
        if ((u16)(ms_tick - dcc_svc.t_last) >= ACK_WAIT_MS)
            op_done();
        return 1;
    default:
        reset_packet();
        return 1;
    }
}

u16 dcc_ack_sample(void)
{
#if MS1_RELEASE == 0x4010
    u16 c = (PORTB & 0x40) ? 0 : 0xFFFF;        /* the comparator: over its threshold */
    u16 base = 0;
#else
    u16 c = track_current, base = dcc_svc.baseline >> 4;
#endif

    if (c >= base + ACK_MIN_MA) {
        if (dcc_svc.above < 0xFF)
            dcc_svc.above++;
        if (dcc_svc.above >= ACK_SAMPLES)
            dcc_svc.ack = 1;
    } else {
        dcc_svc.above = 0;
        dcc_svc.baseline += c - base;
    }
#if MS1_RELEASE != 0x4010
    if (c > LIMIT_MA) {
        if (++dcc_svc.over >= LIMIT_MS) {
            dcc_svc.status = SVC_ABORTED;
            dcc_svc.stop = 1;
            dcc_service_end();
        }
    } else {
        dcc_svc.over = 0;
    }
#endif
    return 0;
}
