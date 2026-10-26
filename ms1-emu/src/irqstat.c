/* irqstat: interrupt timing of a run.
 *
 * Every interrupt is timed from its entry (the stacking included) to the end of its RTI, in
 * bus cycles, nested interrupts included. For the track signal interrupt (timer channel 4,
 * vector 0xFFE6) also, in timer counts (TCNT):
 *   latency  TCNT at entry minus TC4: how long after its compare the interrupt started
 *   slack    TC4 minus TCNT at its RTI: how long before the next compare it finished; the
 *            interrupt has set that compare's levels, so a slack at or below 0 means the
 *            compare passed before it was set up
 *   interval TC4 at this RTI minus TC4 at the previous one: the time between its compares
 * and outside interrupts the longest stretch with interrupts masked (CCR I) after they were
 * first enabled, with the address of its first instruction: it delays every interrupt that
 * comes due meanwhile.
 * At each entry of the track signal interrupt also the fill of the MS1 firmware's staging ring,
 * the timed entries the main loop writes ahead of the interrupt (ms1-firmware
 * docs/architecture.md, Track signal): the entries not yet played and the signal time they
 * hold. The interrupt does not check the ring: when it is empty, it plays an old entry.
 */
#include "irqstat.h"

#include <stdint.h>
#include <string.h>

#define TC4 0x58
#define VEC_TC4 0xFFE6
#define DEPTH 8

/* the staging ring: its read and write pointers; 200 bytes of [control, time] words from 0x1160,
 * in application version 4010 (the version word at 0xF73C) from 0x1155; the low 12 bits of
 * a word are its time in timer counts */
#define STAGING_READ 0x1014
#define STAGING_WRITE 0x1016
#define STAGING_SIZE 200

typedef struct {
	unsigned long count;
	uint64_t total, max;
	uint64_t min;
} vec_t;

static struct {
	vec_t vec[64];
	struct { uint16_t vector; uint64_t start; } stack[DEPTH];
	int depth;
	uint64_t last_cycles;
	/* the track signal interrupt */
	bool have_tc4;
	uint16_t tc4_prev;
	long min_slack, max_latency, min_interval;
	uint64_t min_slack_at;                 /* tick of the RTI with the least slack */
	long min_slack_interval;               /* the interval of that compare */
	unsigned long slack_hist[6];           /* < 0, < 10, < 25, < 50, < 100, >= 100 µs */
	/* interrupts masked outside interrupts */
	bool masked, enabled;
	uint64_t masked_start, masked_max;
	uint16_t masked_pc, masked_max_pc;
	/* the staging ring at the track signal interrupt's entries */
	uint16_t staging_base;
	unsigned long staging_seen, staging_empty;
	unsigned staging_min_entries;
	unsigned long staging_min_counts;
} s;

static const struct { uint16_t vector; const char *name; } names[] = {
	{ 0xFFE6, "timer channel 4: track signal" },
	{ 0xFFEE, "timer channel 0: feedback capture" },
	{ 0xFFD8, "SPI0: display" },
	{ 0xFFCA, "modulus down-counter: 1 ms tick" },
	{ 0xFFB2, "CAN0 receive" },
	{ 0xFFB0, "CAN0 transmit" },
	{ 0xFFB4, "CAN0 errors" },
};

static double counts_us(mcu_t *m, long counts)
{
	return mcu_seconds(m, (uint64_t)(counts < 0 ? -counts : counts) * m->ect.tick_period) * 1e6 *
	       (counts < 0 ? -1 : 1);
}

static uint16_t peek16(mcu_t *m, uint16_t a)
{
	return (uint16_t)(mcu_peek(m, a) << 8 | mcu_peek(m, (uint16_t)(a + 1)));
}

static void staging_sample(mcu_t *m)
{
	uint16_t rd = peek16(m, STAGING_READ), wr = peek16(m, STAGING_WRITE);
	uint16_t lo = s.staging_base, hi = (uint16_t)(lo + STAGING_SIZE);
	if (rd < lo || rd >= hi || wr < lo || wr >= hi || (rd - lo) & 1 || (wr - lo) & 1)
		return;
	unsigned entries = 0;
	unsigned long counts = 0;
	for (uint16_t p = rd; p != wr; p = (uint16_t)(p + 2 == hi ? lo : p + 2)) {
		entries++;
		counts += peek16(m, p) & 0x0FFF;
	}
	if (!s.staging_seen || entries < s.staging_min_entries)
		s.staging_min_entries = entries;
	if (!s.staging_seen || counts < s.staging_min_counts)
		s.staging_min_counts = counts;
	if (!entries)
		s.staging_empty++;
	s.staging_seen++;
}

static void hook(mcu_t *m, uint16_t pc)
{
	cpu12_t *c = &m->cpu;
	if (c->insn_pc != pc) {                                  /* an exception was entered */
		if (s.depth < DEPTH) {
			s.stack[s.depth].vector = c->last_vector;
			s.stack[s.depth].start = s.last_cycles;
			s.depth++;
		}
		if (c->last_vector == VEC_TC4) {
			long lat = (int16_t)(uint16_t)(ect_tcnt(m) - mcu_reg16(m, TC4));
			if (lat > s.max_latency)
				s.max_latency = lat;
			staging_sample(m);
		}
	} else if (mcu_peek(m, pc) == 0x0B && s.depth > 0) {     /* RTI */
		s.depth--;
		uint16_t v = s.stack[s.depth].vector;
		vec_t *e = &s.vec[(v - 0xFF80) / 2 & 63];
		uint64_t d = c->cycles - s.stack[s.depth].start;
		if (!e->count || d < e->min)
			e->min = d;
		if (d > e->max)
			e->max = d;
		e->total += d;
		e->count++;
		if (v == VEC_TC4) {
			uint16_t tc4 = mcu_reg16(m, TC4);
			long slack = (int16_t)(uint16_t)(tc4 - ect_tcnt(m));
			long interval = s.have_tc4 ? (long)(uint16_t)(tc4 - s.tc4_prev) : -1;
			if (interval > 0 && (s.min_interval < 0 || interval < s.min_interval))
				s.min_interval = interval;
			if (!s.have_tc4 || slack < s.min_slack) {
				s.min_slack = slack;
				s.min_slack_at = m->now;
				s.min_slack_interval = interval;
			}
			double us = counts_us(m, slack);
			s.slack_hist[us < 0 ? 0 : us < 10 ? 1 : us < 25 ? 2 : us < 50 ? 3 : us < 100 ? 4 : 5]++;
			s.tc4_prev = tc4;
			s.have_tc4 = true;
		}
	}
	if (!(c->ccr & CCR_I))
		s.enabled = true;
	bool now_masked = s.enabled && s.depth == 0 && (c->ccr & CCR_I);
	if (now_masked && !s.masked) {
		s.masked_start = s.last_cycles;
		s.masked_pc = pc;
	} else if (!now_masked && s.masked) {
		uint64_t d = s.last_cycles - s.masked_start;
		if (d > s.masked_max) {
			s.masked_max = d;
			s.masked_max_pc = s.masked_pc;
		}
	}
	s.masked = now_masked;
	s.last_cycles = c->cycles;
}

void irqstat_attach(mcu_t *m)
{
	memset(&s, 0, sizeof s);
	s.min_interval = -1;
	s.last_cycles = m->cpu.cycles;
	s.staging_base = (mcu_peek(m, 0xF73C) << 8 | mcu_peek(m, 0xF73D)) == 0x4010 ? 0x1155 : 0x1160;
	m->step_hook = hook;
}

void irqstat_print(mcu_t *m, FILE *out)
{
	double cyc_us = 1e6 / mcu_seconds(m, mcu_bus_cycles(m, 1000000)) / 1e6;   /* cycles per µs */
	fprintf(out, "interrupts (bus cycles from entry to the end of RTI, nested ones included; %.0f per µs):\n",
		cyc_us);
	fprintf(out, "  %-6s %-34s %9s %7s %8s %7s\n", "vector", "", "count", "min", "mean", "max");
	for (int i = 0; i < 64; i++) {
		vec_t *e = &s.vec[i];
		if (!e->count)
			continue;
		uint16_t v = (uint16_t)(0xFF80 + 2 * i);
		const char *name = "";
		for (size_t k = 0; k < sizeof names / sizeof names[0]; k++)
			if (names[k].vector == v)
				name = names[k].name;
		fprintf(out, "  0x%04X %-34s %9lu %7llu %8.1f %7llu\n", v, name, e->count,
			(unsigned long long)e->min, (double)e->total / (double)e->count, (unsigned long long)e->max);
	}
	if (s.masked_max)
		fprintf(out, "interrupts masked outside interrupts: at most %llu bus cycles (%.2f µs), from 0x%04X\n",
			(unsigned long long)s.masked_max, (double)s.masked_max / cyc_us, s.masked_max_pc);
	if (s.have_tc4) {
		fprintf(out, "track signal interrupt, in µs: latency max %.2f; slack min %.2f (at %.6f s, compare interval %.2f);"
			" shortest compare interval %.2f\n", counts_us(m, s.max_latency), counts_us(m, s.min_slack),
			mcu_seconds(m, s.min_slack_at), counts_us(m, s.min_slack_interval),
			counts_us(m, s.min_interval));
		fprintf(out, "  slack: < 0 %lu, < 10 µs %lu, < 25 µs %lu, < 50 µs %lu, < 100 µs %lu, >= 100 µs %lu\n",
			s.slack_hist[0], s.slack_hist[1], s.slack_hist[2], s.slack_hist[3], s.slack_hist[4],
			s.slack_hist[5]);
	}
	if (s.staging_seen)
		fprintf(out, "staging ring at the track signal interrupt: at least %u entries, %.0f µs of signal"
			" ahead; empty %lu times\n", s.staging_min_entries,
			counts_us(m, (long)s.staging_min_counts), s.staging_empty);
}
