/* Differential CPU test: execute random instructions on this core and on sim6812's CPU core
 * (its instruction functions, linked in; see the Makefile's diff target) from identical
 * random states, and report every difference in registers, condition codes or memory writes,
 * grouped by opcode. sim6812 implements the M68HC12; differences are adjudicated against the
 * S12CPUV2 Reference Manual (tests/README.md lists the known ones).
 *   diff_sim6812 [iterations-per-opcode] [seed] [v]   (v: also show the known differences)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/cpu12.h"

/* ---- sim6812 side ---- */
#include "Machine.h"
#include "decode.h"
#include "instr.h"

int show_listing = 0;
unsigned short ram_start, eeprom_start, flash_start;

/* ---- shared memory: random background plus a write log per side ---- */

#define MAXW 64
typedef struct { uint16_t addr; uint8_t val; } wr_t;
typedef struct { wr_t w[MAXW]; int n; } wlog_t;

static uint32_t seed_mem;
static uint8_t insn_bytes[8];
static uint16_t insn_pc;
static wlog_t log_ours, log_sim;
static uint8_t fetchbuf[8];
static uint8_t page_ours, page_sim;

static uint8_t background(uint16_t a)
{
	uint32_t h = (a + 0x9E3779B9u) * 0x85EBCA6Bu ^ seed_mem;
	h ^= h >> 13;
	h *= 0xC2B2AE35u;
	h ^= h >> 16;
	return (uint8_t)h;
}

static uint8_t mem_read(wlog_t *l, uint16_t a)
{
	for (int i = l->n - 1; i >= 0; i--)
		if (l->w[i].addr == a)
			return l->w[i].val;
	uint16_t off = (uint16_t)(a - insn_pc);
	if (off < sizeof insn_bytes)
		return insn_bytes[off];
	return background(a);
}

static void mem_write(wlog_t *l, uint16_t a, uint8_t v)
{
	if (l->n < MAXW)
		l->w[l->n++] = (wr_t){ a, v };
}

/* this core's bus */
static uint8_t our_read(void *ctx, uint16_t a) { (void)ctx; return mem_read(&log_ours, a); }
static void our_write(void *ctx, uint16_t a, uint8_t v) { (void)ctx; mem_write(&log_ours, a, v); }
static uint16_t our_irq(void *ctx) { (void)ctx; return 0; }
static uint8_t our_get_ppage(void *ctx) { (void)ctx; return page_ours; }
static void our_set_ppage(void *ctx, uint8_t p) { (void)ctx; page_ours = p; }

/* sim6812's memory interface */
int getmemval(Machine *m, int addr, int sz)
{
	(void)m;
	if (sz == 16)
		return getmemval(m, addr, 8) << 8 | getmemval(m, addr + 1, 8);
	if (sz == 32)
		return getmemval(m, addr, 16) << 16 | getmemval(m, addr + 2, 16);
	return mem_read(&log_sim, (uint16_t)addr);
}

void setmem(Machine *m, int addr, int val, int sz)
{
	(void)m;
	if (sz == 16) {
		setmem(m, addr, val >> 8, 8);
		setmem(m, addr + 1, val, 8);
		return;
	}
	mem_write(&log_sim, (uint16_t)addr, (uint8_t)val);
}

unsigned char *fetch_stream(Machine *m)
{
	for (int i = 0; i < 8; i++)
		fetchbuf[i] = mem_read(&log_sim, (uint16_t)(m->pc + i));
	return fetchbuf;
}

void machine_local_irq(Machine *m) { (void)m; }
/* sim6812's M68HC12 timing table is not compared (and asserts on some modes) */
unsigned int get_timing(unsigned char mode, unsigned char model) { (void)mode; (void)model; return 1; }
void machine_set_page(Machine *m, unsigned char page) { (void)m; page_sim = page; }

/* ---- random instruction generation ---- */

static uint32_t rng = 1;
static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

/* An indexed postbyte plus extension bytes; idx_only limits it to the IDX forms (MOVB/MOVW). */
static int gen_idx(uint8_t *out, bool idx_only, bool no_pc_auto)
{
	for (;;) {
		uint8_t xb = (uint8_t)rnd();
		bool auto_mode = (xb & 0x20) && (xb & 0xE0) != 0xE0;
		if (auto_mode && no_pc_auto && (xb >> 6) == 3)
			continue;
		if ((xb & 0xE0) == 0xE0) {
			bool ext9 = !(xb & 0x04) && !(xb & 0x02);
			bool ext16 = !(xb & 0x04) && (xb & 0x02);
			bool ind_d = (xb & 0x07) == 0x07;
			if (idx_only && (ext9 || ext16 || ind_d))
				continue;
			out[0] = xb;
			if (ext9) { out[1] = (uint8_t)rnd(); return 2; }
			if (ext16) { out[1] = (uint8_t)rnd(); out[2] = (uint8_t)rnd(); return 3; }
			return 1;
		}
		out[0] = xb;
		return 1;
	}
}

/* Encodings not compared: unimplemented here, or changing machine state the harness does not
 * model (interrupt/wait/stop/background, paging). */
static bool skip_opcode(int page, int op)
{
	if (page == 1) {
		switch (op) {
		case 0x00: case 0x01: case 0x0A: case 0x3C: case 0x3E: case 0x3F: case 0x4A: case 0x4B:
		case 0x18:
			return true;
		}
		return false;
	}
	if (op >= 0x30 && op != 0x3E && op != 0x3F) /* TRAP, REV/REVW/WAV, TBL */
		return true;
	return op == 0x12 || op == 0x3D || op == 0x3E || op == 0x3F;
}

/* Build an instruction for page/op into insn_bytes; returns false if none. */
static bool gen_insn(int page, int op)
{
	uint8_t *b = insn_bytes;
	int n = 0;
	for (size_t i = 0; i < sizeof insn_bytes; i++)
		b[i] = (uint8_t)rnd();
	if (page == 2)
		b[n++] = 0x18;
	b[n++] = (uint8_t)op;
	if (page == 1) {
		if (op == 0x04) {                          /* loop primitive: valid lb only */
			uint8_t lb;
			do lb = (uint8_t)rnd(); while ((lb >> 5) > 5 || (lb & 7) == 2 || (lb & 7) == 3 || (lb & 8));
			b[n++] = lb;
			return true;
		}
		if (op == 0xB7) {                          /* TFR/EXG: skip TMP2/TMP3 (reserved) */
			uint8_t eb;
			do eb = (uint8_t)rnd(); while ((eb >> 4 & 7) == 3 || (eb & 7) == 3 || (eb & 8));
			b[n++] = eb;
			return true;
		}
		bool idx = (op >= 0x60 && op <= 0x6F) || (op >= 0xA0 && op <= 0xAF) || (op >= 0xE0 && op <= 0xEF) ||
			op == 0x05 || op == 0x15 || op == 0x19 || op == 0x1A || op == 0x1B || (op >= 0x0C && op <= 0x0F);
		if (idx) {
			bool lea = op == 0x19 || op == 0x1A || op == 0x1B;
			n += gen_idx(b + n, false, lea);
		}
		return true;
	}
	/* page 2 */
	switch (op) {
	case 0x00: case 0x08: n += gen_idx(b + n, true, true); break;         /* #imm -> idx */
	case 0x01: case 0x09: n += gen_idx(b + n, true, true); break;         /* ext -> idx */
	case 0x02: case 0x0A:                                                 /* idx -> idx */
		n += gen_idx(b + n, true, true);
		n += gen_idx(b + n, true, true);
		break;
	case 0x05: case 0x0D: n += gen_idx(b + n, true, true); break;         /* idx -> ext */
	default:
		if (op >= 0x18 && op <= 0x1F)
			n += gen_idx(b + n, false, true);
	}
	return true;
}

/* ---- comparison ---- */

/* Opcodes where sim6812 departs from the S12CPUV2 manual, with the evidence (its instr.c). */
static const struct { int page, op; const char *why; } known[] = {
	{ 1, 0x0B, "RTI: sim6812 lets the stacked CCR set X (manual: X cannot change 0->1)" },
	{ 1, 0x38, "PULC: as RTI, X set from the stack" },
	{ 2, 0x10, "IDIV by zero: sim6812 leaves X (manual: quotient $FFFF)" },
	{ 2, 0x14, "EDIVS: sim6812 writes Y/D on overflow (manual: unchanged)" },
	{ 1, 0x11, "EDIV: sim6812 writes Y/D on overflow (manual: unchanged)" },
	{ 1, 0x43, "DECA: sim6812 never clears V (manual: V = result is $7F)" },
	{ 1, 0x53, "DECB: as DECA" },
	{ 1, 0x63, "DEC idx: as DECA" },
	{ 1, 0x73, "DEC ext: as DECA" },
	{ 1, 0x47, "ASRA: sim6812 shifts in 0 (manual: bit 7 held)" },
	{ 1, 0x57, "ASRB: as ASRA" },
	{ 1, 0x67, "ASR idx: as ASRA" },
	{ 1, 0x77, "ASR ext: as ASRA" },
	{ 1, 0x55, "ROLB: sim6812 tests Z on the unmasked 9-bit value ($80 with C=0 gives Z=0)" },
	{ 1, 0x8F, "CPS: sim6812 compares B instead of SP" },
	{ 1, 0x9F, "CPS: as above" },
	{ 1, 0xAF, "CPS: as above" },
	{ 1, 0xBF, "CPS: as above" },
	{ 1, 0xB7, "EXG with CCR: sim6812 lets X be set" },
	{ 2, 0x07, "DAA: sim6812 changes H (manual: H unaffected)" },
	{ 2, 0x18, "MAXA: sim6812 takes N/V of an 8-bit compare from bit 15" },
	{ 2, 0x19, "MINA: sim6812 decides on N instead of C, flags from bit 15" },
	{ 2, 0x1A, "EMAXD: sim6812 stores D - M and takes N/Z from D" },
	{ 2, 0x1B, "EMIND: as EMAXD" },
	{ 2, 0x1C, "MAXM: sim6812 decides on N, flags from bit 15" },
	{ 2, 0x1D, "MINM: as MAXM" },
	{ 2, 0x1E, "EMAXM: sim6812 stores D - M" },
	{ 2, 0x1F, "EMINM: as EMAXM" },
};

static const char *known_bug(int page, int op)
{
	for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
		if (known[i].page == page && known[i].op == op)
			return known[i].why;
	return 0;
}

typedef struct { int fails; int runs; char example[400]; } stat_t;
static stat_t stats[2][256];

static bool logs_equal(void)
{
	/* compare final contents at every written address */
	for (int s = 0; s < 2; s++) {
		wlog_t *l = s ? &log_sim : &log_ours;
		for (int i = 0; i < l->n; i++) {
			uint16_t a = l->w[i].addr;
			if (mem_read(&log_ours, a) != mem_read(&log_sim, a))
				return false;
		}
	}
	return true;
}

/* Condition-code bits the manual leaves undefined for an opcode. */
static uint8_t undefined_ccr(int page, int op, const cpu12_t *c)
{
	if (page == 2 && op == 0x07)
		return CCR_V;                                         /* DAA */
	if (page == 1 && op == 0x11)                              /* EDIV: after overflow or /0 */
		return (c->ccr & (CCR_V | CCR_C)) ? CCR_N | CCR_Z | ((c->ccr & CCR_C) ? CCR_V : 0) : 0;
	if (page == 2 && (op == 0x14 || op == 0x15))              /* EDIVS / IDIVS */
		return (c->ccr & (CCR_V | CCR_C)) ? CCR_N | CCR_Z | ((c->ccr & CCR_C) ? CCR_V : 0) : 0;
	return 0;
}

int main(int argc, char **argv)
{
	int iters = argc > 1 ? atoi(argv[1]) : 2000;
	rng = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 0) : 12345;
	bool verbose = argc > 3;
	cpu12_t c;
	Machine m;
	instr_info ii;
	mode_info m1, m2;
	struct _Memory simmem;

	memset(&c, 0, sizeof c);
	c.bus = (cpu12_bus_t){ 0, our_read, our_write, our_irq, 0, our_get_ppage, our_set_ppage };
	memset(&m, 0, sizeof m);
	memset(&simmem, 0, sizeof simmem);
	m.mem = &simmem;
	m.ns_per_cycle = 1;
	ii.mode1 = &m1;
	ii.mode2 = &m2;

	for (int page = 1; page <= 2; page++) {
		for (int op = 0; op < 256; op++) {
			if (skip_opcode(page, op))
				continue;
			for (int it = 0; it < iters; it++) {
				gen_insn(page, op);
				seed_mem = rnd();
				insn_pc = (uint16_t)rnd();
				log_ours.n = log_sim.n = 0;
				c.a = (uint8_t)rnd(); c.b = (uint8_t)rnd();
				c.x = (uint16_t)rnd(); c.y = (uint16_t)rnd(); c.sp = (uint16_t)rnd();
				c.ccr = (uint8_t)(rnd() | CCR_I | CCR_S);  /* no pending-interrupt effects */
				c.pc = insn_pc;
				c.state = CPU12_RUN;
				c.rti_chain = c.irq_delay = false;
				m.pc = c.pc; m.sp = c.sp; m.x = c.x; m.y = c.y; m.d = cpu12_d(&c); m.ccr = c.ccr;
				m.irq_pending = 0;
				page_ours = page_sim = 0x3D;
				simmem.page = 0x3D;
				char before[200];
				snprintf(before, sizeof before, "A=%02X B=%02X X=%04X Y=%04X SP=%04X CCR=%02X", c.a, c.b, c.x, c.y,
					c.sp, c.ccr);
				uint8_t ib[8];
				memcpy(ib, insn_bytes, 8);

				cpu12_step(&c);
				decode(&m, &ii);
				m.pc = (unsigned short)(m.pc + ii.len);  /* sim6812 execute() */
				ii.fn(&m, &ii);

				uint8_t undef = undefined_ccr(page, op, &c);
				bool regs_ok = c.a == (uint8_t)(m.d >> 8) && c.b == (uint8_t)m.d && c.x == m.x && c.y == m.y &&
					c.sp == m.sp && c.pc == m.pc && ((c.ccr ^ m.ccr) & ~undef) == 0 && c.state != CPU12_HALT;
				bool mem_ok = logs_equal();
				stat_t *st = &stats[page - 1][op];
				st->runs++;
				if (!regs_ok || !mem_ok) {
					if (!st->fails++) {
						char dis[64];
						cpu12_disasm(our_read, 0, insn_pc, dis, sizeof dis);
						snprintf(st->example, sizeof st->example,
							"  @%04X %02X %02X %02X %02X %02X %02X  %s\n    before %s\n    ours   A=%02X B=%02X X=%04X Y=%04X SP=%04X PC=%04X CCR=%02X%s\n    sim    A=%02X B=%02X X=%04X Y=%04X SP=%04X PC=%04X CCR=%02X%s",
							insn_pc, ib[0], ib[1], ib[2], ib[3], ib[4], ib[5], dis, before,
							c.a, c.b, c.x, c.y, c.sp, c.pc, c.ccr, c.state == CPU12_HALT ? " HALT" : "",
							(uint8_t)(m.d >> 8), (uint8_t)m.d, m.x, m.y, m.sp, m.pc, m.ccr,
							mem_ok ? "" : "  (memory writes differ)");
					}
				}
			}
		}
	}
	int bad = 0, expected = 0, compared = 0;
	for (int page = 1; page <= 2; page++)
		for (int op = 0; op < 256; op++) {
			stat_t *st = &stats[page - 1][op];
			compared += st->runs > 0;
			if (!st->fails)
				continue;
			const char *why = known_bug(page, op);
			if (why) {
				expected++;
				if (verbose)
					printf("%s%02X: %d/%d differ (known: %s)\n%s\n", page == 2 ? "18 " : "", op, st->fails,
						st->runs, why, st->example);
				continue;
			}
			bad++;
			printf("%s%02X: %d/%d differ\n%s\n", page == 2 ? "18 " : "", op, st->fails, st->runs, st->example);
		}
	printf("%d opcodes compared, %d iterations each: %d differ as listed (sim6812 bugs), %d unexplained\n",
		compared, iters, expected, bad);
	return bad != 0;
}
