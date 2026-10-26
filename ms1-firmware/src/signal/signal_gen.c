/*==========================================================================
 * signal/signal_gen.c - the track signal generator: the timer channel 4 interrupt and the
 * main-loop stages that feed it
 *
 * Command entries (track_cmd.h) become [opcode, data] pairs in the send buffer
 * (signal_gen_cmd_buf_consumer), the pairs become timed entries in the staging ring
 * (signal_gen_send_buf_executor), and the timer channel 4 interrupt plays the entries out on
 * the track outputs. Entry types, opcodes and entry bits: docs/architecture.md, Track signal.
 *
 * Functions:
 *   isr_timer_ch4_signal_gen     (0xC4ED) - interrupt: the next staging entry
 *   signal_gen_init              (0xC58E) - fill the stages with pauses, start the interrupt
 *   signal_gen_send_buf_executor (0xC614) - up to count pairs into staging entries
 *   signal_gen_cmd_buf_consumer  (0xC81D) - up to count command entry steps into pairs
 *   track_cmd_pause              (0xCA46) - a type 1 entry
 *   track_cmd_write_type2        (0xCA5E) - a type 2 entry
 *   track_cmd_advance            (0xCA6B) - close the entry being written
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/asm.h"
#include "../include/globals.h"
#include "../include/rom_tables.h"
#include "../include/track_cmd.h"

#define STAGING_END     (signal_staging_buf + sizeof signal_staging_buf)
#define SEND_END        (signal_gen_send_buffer + sizeof signal_gen_send_buffer)
#define CMD_END         (track_cmd_buf + sizeof track_cmd_buf)
#define CMD_ENTRY_SIZE  12

/* staging entry levels (control byte bits 4, 5 and 7) and timer ticks (6 per µs) */
#define POS             0x1000
#define NEG             0x2000
#define MFX_FULL        0x258
#define MFX_HALF        0x12C
#define MM_LONG         0x444
#define MM_SHORT        0x09C

#define PAIR(op, d)     ((u16)(op) << 8 | (u8)(d))

/* At each compare of timer channel 4 the outputs take the levels the last pass set; a pass
 * sets those of the entry at the read position for the next compare, which it puts that
 * entry's time later (TC4, TC5 the track outputs, 4011 also TC3 the booster line). An entry
 * with time 0 only sets its levels, and the next entry follows at once. An entry with bit 6
 * runs track_capture_cmd (signal/feedback_capture.c), then takes track_capture_next as the
 * next command. The ring has no empty check: the main-loop stages keep it filled (README:
 * Interrupt timing). Assembly: its time to the next compare is short, and the moment at
 * which a capture starts counts in cycles. */
__asm__ (
"	.sect	.text.isr_timer_ch4_signal_gen\n"
"	.globl	isr_timer_ch4_signal_gen\n"
"isr_timer_ch4_signal_gen:\n"
"	ldx	signal_gen_staging_read_ptr\n"
".Lch4_entry:\n"
#if MS1_RELEASE == 0x4010
"	brclr	0,x,#0x80,.Lch4_booster\n"     /* bit 7: PB7 low */
"	bclr	PORTB,#0x80\n"
"	bra	.Lch4_pos\n"
".Lch4_booster:\n"
"	bset	PORTB,#0x80\n"
#else
"	brclr	0,x,#0x80,.Lch4_booster\n"     /* bit 7: channel 3 high */
"	bset	TCTL2,#0x40\n"
"	bra	.Lch4_pos\n"
".Lch4_booster:\n"
"	bclr	TCTL2,#0x40\n"
#endif
".Lch4_pos:\n"
"	brclr	0,x,#0x10,.Lch4_pos_low\n"     /* bit 4: channel 4 high */
"	bset	TCTL1,#1\n"
"	bra	.Lch4_neg\n"
".Lch4_pos_low:\n"
"	bclr	TCTL1,#1\n"
".Lch4_neg:\n"
"	brclr	0,x,#0x20,.Lch4_neg_low\n"     /* bit 5: channel 5 high */
"	bset	TCTL1,#4\n"
"	bra	.Lch4_capture\n"
".Lch4_neg_low:\n"
"	bclr	TCTL1,#4\n"
".Lch4_capture:\n"
"	brclr	0,x,#0x40,.Lch4_compare\n"
"	pshx\n"
"	ldaa	track_capture_cmd\n"
"	cmpa	#1\n"
"	bne	.Lch4_cmd2\n"
"	jsr	timer_capture_start_bit_mode\n"
"	bra	.Lch4_captured\n"
".Lch4_cmd2:\n"
"	cmpa	#2\n"
"	bne	.Lch4_cmd81\n"
"	jsr	timer_capture_start_adc_mode\n"
"	bra	.Lch4_captured\n"
".Lch4_cmd81:\n"
"	cmpa	#0x81\n"
"	bne	.Lch4_cmd82\n"
"	jsr	timer_capture_stop_and_align\n"
"	std	track_capture_samples\n"
"	bra	.Lch4_captured\n"
".Lch4_cmd82:\n"
"	cmpa	#0x82\n"
"	bne	.Lch4_captured\n"
"	jsr	timer_capture_stop_and_count\n"
"	std	track_capture_samples\n"
".Lch4_captured:\n"
"	pulx\n"
"	movb	track_capture_next,track_capture_cmd\n"
"	clr	track_capture_next\n"
".Lch4_compare:\n"
"	movw	signal_timer_compare_base,TC4\n"
"	movw	signal_timer_compare_base,TC5\n"
#if MS1_RELEASE != 0x4010
"	movw	signal_timer_compare_base,TC3\n"
#endif
"	ldd	2,x+\n"
"	anda	#15\n"                         /* the time: bits 0-11 */
"	cpd	#0\n"
"	beq	.Lch4_now\n"
"	addd	signal_timer_compare_base\n"
"	std	signal_timer_compare_base\n"
"	cpx	#signal_staging_buf+" ASM_NUM(SIGNAL_STAGING_SIZE) "\n"
"	bne	.Lch4_done\n"
"	ldx	#signal_staging_buf\n"
".Lch4_done:\n"
"	stx	signal_gen_staging_read_ptr\n"
"	rti\n"
".Lch4_now:\n"
"	cpx	#signal_staging_buf+" ASM_NUM(SIGNAL_STAGING_SIZE) "\n"
"	lbne	.Lch4_entry\n"
"	ldx	#signal_staging_buf\n"
"	jmp	.Lch4_entry\n"
);

/* The entry at the write position. An entry with one track output high makes the other the
 * level of the next mfx edge. */
static void signal_gen_staging_buf_write(u16 entry)
{
    volatile u16 *p = (volatile u16 *)signal_gen_staging_write_ptr;

    *p++ = entry;
    if (entry & NEG) {
        if (!(entry & POS))
            track_polarity = 0x10;
    } else if (entry & POS)
        track_polarity = 0x20;
    if ((u8 *)p == STAGING_END)
        p = (volatile u16 *)signal_staging_buf;
    signal_gen_staging_write_ptr = (u16)p;
}

static void signal_gen_fullbit_entry(void)
{
    mfx_stuff_count = 8;
    signal_gen_staging_buf_write((u16)track_polarity << 8 | MFX_FULL);
}

static void signal_gen_halfbit_entry(void)
{
    signal_gen_staging_buf_write((u16)track_polarity << 8 | MFX_HALF);
}

/* an mfx 1, and the 0 stuffed after eight of them */
static void signal_gen_halfbit_pair(void)
{
    signal_gen_halfbit_entry();
    signal_gen_halfbit_entry();
    if (--mfx_stuff_count == 0)
        signal_gen_fullbit_entry();
}

/* half the mfx sync pattern */
static void signal_gen_idle_pulse(void)
{
    signal_gen_fullbit_entry();
    signal_gen_halfbit_entry();
    signal_gen_fullbit_entry();
}

/* MM pause units of 24000/39 ticks, 1-4 per entry */
static const u16 mm_pause_ticks[4] = {
    24000UL * 1 / 39, 24000UL * 2 / 39, 24000UL * 3 / 39, 24000UL * 4 / 39
};

void signal_gen_send_buf_executor(u16 count)
{
    u8 *x;
    s16 room;
    u8 d, n, u;
    u16 e;

    while (count--) {
        x = (u8 *)signal_gen_send_buf_read_ptr;
        if ((u16)x == signal_gen_send_buf_write_ptr)
            return;
        /* room for the most one pair writes */
        room = signal_gen_staging_read_ptr - signal_gen_staging_write_ptr;
        if (room != 0) {
            if (room < 0)
                room += sizeof signal_staging_buf;
            if ((u16)room <= 0x30)
                return;
        }
        switch (*x++) {
        case 0x01:                      /* track off */
            e = 0x8000 | (u16)*x << 4;
            for (n = 4; n; n--)
                signal_gen_staging_buf_write(e);
            break;
        case 0x02:                      /* capture control */
            if (track_capture_cmd == 0) {
                track_capture_cmd = *x;
                track_capture_next = 0;
            } else
                track_capture_next = *x;
            signal_gen_staging_buf_write(0x4000);
            break;
        case 0x03: case 0x04: case 0x05: case 0x06:
        case 0x07: case 0x08: case 0x09: case 0x0A:  /* the top 1-8 bits as mfx bits */
            n = x[-1] - 2;
            for (d = *x; n; n--, d <<= 1) {
                if (d & 0x80)
                    signal_gen_halfbit_pair();
                else
                    signal_gen_fullbit_entry();
            }
            break;
        case 0x0B:                      /* half sync patterns */
            if (*x & 0x80) {
                d = *x & 0x7F;
                if (!(track_polarity & 0x20))
                    d++;
                *x = d;
            }
            do
                signal_gen_idle_pulse();
            while (--*x);
            break;
        case 0x0E:                      /* MM pause */
            for (d = *x; d; d -= u) {
                u = d & 3;
                if (u == 0)
                    u = 4;
                signal_gen_staging_buf_write(NEG | mm_pause_ticks[u - 1]);
            }
            break;
        case 0x0F:                      /* MM bits: the top 8 */
        case 0x10:                      /* the top 2 */
            n = x[-1] == 0x0F ? 8 : 2;
            for (d = *x; n; n--, d <<= 1) {
                if (d & 0x80) {
                    signal_gen_staging_buf_write(POS | MM_LONG);
                    signal_gen_staging_buf_write(NEG | MM_SHORT);
                } else {
                    signal_gen_staging_buf_write(POS | MM_SHORT);
                    signal_gen_staging_buf_write(NEG | MM_LONG);
                }
            }
            break;
        case 0x11:
            for (n = 6; n; n--) {
                d = *x << 1;
                if (d)
                    signal_gen_staging_buf_write((*x & 0x80 ? POS : NEG) | (u16)d << 2);
                d = 254 - d;
                if (d)
                    signal_gen_staging_buf_write((u16)d << 2);
            }
            break;
        case 0x12:                      /* an edge, d µs */
            signal_gen_staging_buf_write((u16)track_polarity << 8 | *x * 6);
            break;
        case 0x13:
            e = (u16)track_polarity << 8 | *x * 12;
            signal_gen_staging_buf_write(e);
            signal_gen_staging_buf_write(e);
            break;
        case 0x14:                      /* d µs at the present level */
            signal_gen_staging_buf_write(((u16)track_polarity << 8 | *x * 6) ^ (POS | NEG));
            break;
        case 0x15:
            e = ((u16)track_polarity << 8 | *x * 12) ^ (POS | NEG);
            signal_gen_staging_buf_write(e);
            signal_gen_staging_buf_write(e);
            break;
        }
        if (++x == SEND_END)
            x = signal_gen_send_buffer;
        signal_gen_send_buf_read_ptr = (u16)x;
    }
}

/* A pair at the send buffer's write position */
static void send_pair(u16 pair)
{
    u16 *p = (u16 *)signal_gen_send_buf_write_ptr;

    *p++ = pair;
    if ((u8 *)p == SEND_END)
        p = (u16 *)signal_gen_send_buffer;
    signal_gen_send_buf_write_ptr = (u16)p;
}

/* After an mfx entry (4, 5): two mfx sync patterns before any but another */
static void signal_gen_filter_aux_cmds(u8 type)
{
    if (type != 4 && type != 5 && type != 3)
        send_pair(PAIR(0x0B, 4));
}

/* the opcodes of an MM entry's steps: pause, address, function, data, then again */
static const u8 mm_step_ops[8] = { 0x0E, 0x0F, 0x10, 0x0F, 0x0E, 0x0F, 0x10, 0x0F };

/* The step track_cmd_pos of entry e */
static void cmd_step(u8 *e)
{
    u8 pos = track_cmd_pos;
    u8 n;

    switch (e[0]) {
    case 1:                             /* pause: 0x80 per step, the remainder last */
        send_pair(PAIR(1, (u8)(pos + 1) == e[1] ? e[2] : 0x80));
        break;
    case 2:
        send_pair(PAIR(2, e[2]));
        break;
    case 3:
        send_pair(PAIR(0x0B, 4));
        break;
    case 4:                             /* an mfx frame, a byte per step */
        if (pos == 0)
            send_pair(PAIR(0x0B, 2));
        if ((u8)((e[1] & 15) - 1) == pos) {
            /* the last byte: its bit count from the high nibble of e[1] */
            n = e[1];
            e[1] = pos + 1;
            send_pair(PAIR((n >> 4) + 2, e[(u8)(pos + 2)]));
        } else
            send_pair(PAIR(0x0A, e[(u8)(pos + 2)]));
        break;
    case 5:                             /* a feedback window, mode e[2] */
        if (pos == 0) {
            send_pair(PAIR(0x0B, 0x82));
            send_pair(PAIR(0x06, 0x30));
            send_pair(PAIR(2, e[2]));
            if (e[2] == 2)
                send_pair(PAIR(0x12, 10));
        } else if (e[2] == 2) {
            if ((u8)(pos * 2 - 1) == e[1]) {
                send_pair(PAIR(0x0B, 3));
                send_pair(PAIR(0x12, 10));
            }
            send_pair(PAIR(0x15, 0xE4));
        } else if (pos >= 0x18) {
            send_pair(PAIR(0x13, 0x6B));
            send_pair(PAIR(0x13, 0x07));
            send_pair(PAIR(0x13, 0x6B));
            send_pair(PAIR(0x13, 0x07));
        } else {
            send_pair(PAIR(0x13, 0xDD));
            send_pair(PAIR(0x13, 0x07));
        }
        if ((u8)(pos + 1) == e[1])
            send_pair(PAIR(2, e[2] | 0x80));
        break;
    case 8:
        send_pair(PAIR(0x0E, e[2]));
        break;
    case 9:                             /* an MM double packet */
        send_pair(PAIR(mm_step_ops[pos], e[(u8)(pos + 2)]));
        break;
    case 12:
        send_pair(PAIR(0x11, e[2]));
        break;
    /* the original's jump table has no handler for the other types: they write nothing here */
    }
}

void signal_gen_cmd_buf_consumer(u16 count)
{
    u8 *e;
    s16 room;
    u8 type, pos;

    while (count--) {
        /* room for the most one step writes */
        room = signal_gen_send_buf_read_ptr - signal_gen_send_buf_write_ptr;
        if (room != 0) {
            if (room < 0)
                room += sizeof signal_gen_send_buffer;
            if ((u16)room <= 10)
                return;
        }
        e = (u8 *)track_cmd_read_ptr;
        if (track_cmd_free == 4)
            return;
        type = e[0];
        if (type != signal_gen_prev_cmd_type && type != 1 && type != 2 && type != 12
            && type != 13) {
            if (signal_gen_prev_cmd_type == 4 || signal_gen_prev_cmd_type == 5)
                signal_gen_filter_aux_cmds(type);
            else if (signal_gen_prev_cmd_type == 9 && (type < 8 || type > 10))
                send_pair(PAIR(0x0E, 0x3C));    /* an MM pause after MM */
            signal_gen_prev_cmd_type = type;
        }
        cmd_step(e);
        pos = track_cmd_pos + 1;
        if (pos == e[1]) {
            pos = 0;
            e += CMD_ENTRY_SIZE;
            if (e == CMD_END)
                e = track_cmd_buf;
            track_cmd_read_ptr = (u16)e;
            track_cmd_free++;
        }
        track_cmd_pos = pos;
    }
}

/* About timing_value µs at 0 V: 1365 µs (0x80 × 64 ticks) per step, then the remainder */
void track_cmd_pause(u16 timing_value)
{
    u16 q = (u16)((u32)timing_value * 6000 / 32000);
    u8 *e = (u8 *)track_cmd_write_ptr;

    e[0] = 1;
    e[1] = (q >> 8) + 1;
    e[2] = (u8)q >> 1;
    track_cmd_advance();
}

void track_cmd_write_type2(u8 data)
{
    u8 *e = (u8 *)track_cmd_write_ptr;

    e[0] = 2;
    e[1] = 1;
    e[2] = data;
    track_cmd_advance();
}

void track_cmd_advance(void)
{
    u8 *e = (u8 *)track_cmd_write_ptr + CMD_ENTRY_SIZE;

    if (e == CMD_END)
        e = track_cmd_buf;
    track_cmd_write_ptr = (u16)e;
    track_cmd_free--;
}

/* 1 ms track-off pauses through all three stages, so that the track starts at 0 V; then
 * timer channels 4 and 5 as output compares (PT4, PT5), the timer at 6 MHz and the
 * channel 4 interrupt on. Version 4010 also sets the booster's over-current threshold. */
void signal_gen_init(void)
{
    u8 n;

#if MS1_RELEASE == 0x4010
    TIE &= ~0x30;
    PORTB &= ~0x80;
#else
    TIE &= ~0x38;
    TIOS &= ~0x38;
    PTT &= ~0x30;
    PTT |= 0x08;
    DDRT |= 0x38;
#endif
    for (n = 13; n; n--) {
        track_cmd_pause(1000);
        signal_gen_cmd_buf_consumer(1);
        signal_gen_send_buf_executor(4);
    }
    for (n = 28; n; n--) {
        track_cmd_pause(1000);
        signal_gen_cmd_buf_consumer(1);
    }
    for (n = 3; n; n--)
        track_cmd_pause(1000);
#if MS1_RELEASE == 0x4010
    /* PWM channels 0-1 and 2-3 concatenated, centre-aligned */
    PWMCTL |= 0x30;
    PWMCAE |= 0x0A;
    PWMPOL |= 0x0A;
    PWMPER01 = 0xFFF;
    PWMPER23 = 0xFFF;
    PWMDTY23 = (u16)((u32)flash_current_limit * (u16)(flash_current_limit + 1000) / 1550);
    PWMDTY01 = 39;
    PWME |= 0x0A;
    TIOS |= 0x30;
    TC4 = 0;
    TC3 = 0;
    TCTL1 |= 0x02;
    TCTL1 &= ~0x01;
    TCTL1 |= 0x0A;
    TCTL1 &= ~0x05;
    TSCR2 = 2;
    TSCR1 = 0x90;
    PTT &= ~0x30;
    PORTB &= ~0x80;
    DDRB |= 0x80;
    DDRT |= 0x30;
#else
    TIOS |= 0x30;
    TC5 = 0;
    TC4 = 0;
    TC3 = 0;
    TCTL2 |= 0xC0;
    TCTL1 |= 0x0A;
    TCTL1 &= ~0x05;
    TSCR2 = 2;
    TSCR1 = 0x90;
#endif
    TIE |= 0x10;
}
