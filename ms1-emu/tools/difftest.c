/* difftest: function-level differential test of a firmware rebuild against the original.
 *
 * The original image runs a scenario (time, inputs) on the emulator. Each time it enters a
 * function listed in the function table, the whole machine (MCU and board) is copied twice:
 * one copy runs the original function, the other the rebuild's function from the same state
 * (the rebuild's flash swapped in, PC at its entry), both with interrupts held off and the
 * board's inputs held still, until the function returns to its caller. The two outcomes are then compared: return value (when the
 * table gives the function one), RAM (the variables up to the GCC soft registers, and the
 * caller's stack from the return address up), EEPROM (with the commands under way completed),
 * and the sequence of writes to the I/O registers. This needs the rebuild's RAM variables at the original's addresses and the same
 * calling convention (GCC m68hc1x: first argument in D, the rest on the stack).
 *
 *   difftest [options] ORIGINAL.s19 REBUILD.s19 SYMBOLS FUNCTIONS
 *     -t SECONDS   scenario length (default 15)
 *     -s FILE      scenario inputs, as ms1emu -s: 'SECONDS press|release BUTTON', 'SECONDS turn N',
 *                  'SECONDS unplug|plug', 'SECONDS text' (print the LCD text)
 *     -a CH=VALUE  ATD0 channel input (as ms1emu)
 *     -e FILE      EEPROM contents (1024 bytes)
 *     -n N         calls tested per function (default 20)
 *     -m MS        time limit per call, emulated milliseconds (default 50)
 *     -v           print every mismatch, not just the first per function, and every
 *                  differing RAM byte, not just the first 12
 *     -f NAME      test only this function (repeatable)
 *     -x           with a mismatch, print the addresses the rebuild executed inside the tested
 *                  function (the last 4096 instructions), to follow its path in a disassembly
 *     -r ADDR:COUNT a ring of COUNT CAN frames at ADDR, each as the MSCAN buffers hold one
 *                  (identifier 4 bytes, data 8, length 1): the data bytes past a frame's
 *                  length are never sent and not compared (repeatable)
 *     -M IMAGE     a second MS1 running IMAGE on the same CAN bus (e.g. the original as master,
 *                  with the tested one started as slave by -a 0=0); the two run in lockstep,
 *                  0.1 ms apart at most
 *     -S FILE      its scenario inputs
 *     -A CH=VALUE  its ATD0 channel input (e.g. -A 0=0 to start it as slave)
 *     -D SPEC      a locomotive decoder on the original's track (as ms1emu; repeatable)
 *     -u NAME[:N]  after the scenario, call the listed function directly N times (default 50)
 *                  from the machine state then, with varied inputs: D, X, Y and the stack
 *                  arguments in turn a small number, a RAM address or a word below the flash
 *                  (a fixed pseudo-random sequence); for functions no scenario reaches
 *                  (repeatable)
 *     -c FILE      log the CAN frames of the machine (as ms1emu -c)
 *     -y           after the table, the mean time per tested call of each function in the
 *                  original and in the rebuild (emulated microseconds, interrupts held off)
 *     -W           whole firmware: run REBUILD.s19 as the machine and test no function, for
 *                  comparing what a scenario shows (text lines, decoder events, -c log) with
 *                  a normal run of the original; its LCD is read back with the tables
 *                  SYMBOLS names
 *     -I           with -W: interrupt timing after the run (as ms1emu -I, src/irqstat.c)
 *     -B SECONDS   start the application of each machine (this one and -M's) no earlier than
 *                  SECONDS after power-on (as ms1emu -B): the original and a rebuild then start
 *                  at the same moment, whatever the bootloader's CRC check over their flash takes
 *   SYMBOLS: the rebuild's symbol table as `nm -n` prints it ('ADDR TYPE NAME'): function
 *   entries, and the names of RAM variables for the report. A listed function missing from
 *   it (not linked into the rebuild) is reported and not tested.
 *   FUNCTIONS: lines 'ORIG_ADDR NAME [v|b|w [ARGBYTES [pN]]]': the original's entry address
 *   (hex), the rebuild's function, its return value (none, byte in B, word in D; default
 *   none), and the bytes of its stack arguments (default 0); '#' starts a comment. The
 *   stack argument slots belong to the called function and are dead after it returns, so
 *   the given bytes above the return address are not compared: a function may keep a
 *   parameter there (the original does where GCC spilled it) or in a register. pN: D and
 *   the first N bytes of stack arguments may point to strings in flash; a call where one
 *   reaches bytes the rebuild's image does not have at that address (the original's
 *   read-only data, which the rebuild keeps elsewhere) is skipped, and counted as such.
 */
#define _POSIX_C_SOURCE 200809L
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/board.h"
#include "../src/irqstat.h"
#include "../src/mcu.h"

#define MAXF 1024
/* the end of the RAM variables, where the GCC soft registers _.tmp, _.z, _.xy, _.frame
 * follow: 0x1BB7 in application version 4011, 0x1BAC in 4010 (the original's version word at
 * 0xF73C) */
static unsigned ram_vars_end = 0x1BB7;
#define LOGMAX 4096

typedef struct {
	uint16_t orig, rebuild;
	char name[64];
	char ret;                  /* 'v', 'b', 'w' */
	unsigned argbytes;         /* stack argument bytes, not compared */
	int flash_args;            /* pN: D and N stack argument bytes may point into flash data; -1 none */
	bool enabled;
	unsigned calls, direct, tested, ok, differ, timeout, rebuild_timeout, skipped;
	double us_orig, us_rebuild;   /* time in the calls that both returned (-y) */
	unsigned timed;
	bool reported;
} func_t;

static func_t funcs[MAXF];
static int nfuncs;
static func_t *by_addr[65536];

static uint8_t rebuild_flash[FLASH_PAGES][PAGE_SIZE];
static int max_tests = 20;
static double limit_ms = 50;
static bool verbose, show_path, show_times;

/* CAN frame rings (-r): data bytes past a frame's length are not compared */
#define FRAME_SIZE 13
static struct { uint16_t addr, count; } rings[4];
static int nrings;

static bool past_frame_length(const mcu_t *m, unsigned a)
{
	for (int i = 0; i < nrings; i++) {
		unsigned off = a - rings[i].addr;
		if (a < rings[i].addr || off >= (unsigned)rings[i].count * FRAME_SIZE)
			continue;
		unsigned entry = off / FRAME_SIZE, k = off % FRAME_SIZE;
		uint8_t len = m->ram[rings[i].addr + entry * FRAME_SIZE + 12 - 0x1000] & 0x0F;
		return k >= 4 && k < 12 && k - 4 >= len;
	}
	return false;
}

/* RAM variable names: the rebuild's symbols at RAM addresses (they are the original's) */
static char *ram_name[0x1000];

/* The rebuild's symbols outside RAM */
typedef struct { char name[64]; uint16_t addr; } sym_t;
static sym_t *syms;
static int nsyms;

typedef struct {
	uint16_t addr[LOGMAX];
	uint8_t val[LOGMAX];
	int n;
	bool overflow;
} iolog_t;

static void io_hook(mcu_t *m, uint16_t addr, uint8_t v)
{
	iolog_t *l = m->hook_ctx;
	if (addr >= 0x400)
		return;
	if (l->n == LOGMAX) {
		l->overflow = true;
		return;
	}
	l->addr[l->n] = addr;
	l->val[l->n++] = v;
}

static void clone(mcu_t *dst, ms1_board_t *bdst, const mcu_t *src, const ms1_board_t *bsrc)
{
	*dst = *src;
	*bdst = *bsrc;
	bdst->m = dst;
	bdst->can_log = 0;
	bdst->track_log = 0;
	bdst->bus = 0;
	bdst->peer = 0;
	bdst->ndecoders = 0;
	dst->cpu.bus.ctx = dst;
	dst->board.ctx = bdst;
	dst->breakpoints = 0;
	dst->log = 0;
	/* The board's inputs hold still: a knob transition or feedback clock edge inside a call
	 * would reach only the side that reads the port after it. Not UINT64_MAX, which a track
	 * output write takes for a feedback clock not yet started. */
	bdst->enc_next = bdst->poll_next = bdst->rds_next = UINT64_MAX - 1;
	mcu_schedule(dst, EV_BOARD, UINT64_MAX - 1);
}

static uint16_t rd16(mcu_t *m, uint16_t a) { return (uint16_t)(mcu_peek(m, a) << 8 | mcu_peek(m, (uint16_t)(a + 1))); }

/* -u: functions called directly after the scenario */
#define DIRECT_RETURN 0xF800   /* return address of a direct call: the bootloader, never reached */
static struct { const char *name; int count; } direct[32];
static int ndirect;
static uint32_t rng = 0x2545F491;

/* An input word, by k: a small number, a RAM address, or any word below the flash (whose
 * contents differ between the images, so a pointer there would read different data). */
static uint16_t input_word(unsigned k)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	uint16_t r = (uint16_t)(rng >> 8);
	switch (k % 3) {
	case 0: return r % 24;
	case 1: return (uint16_t)(0x1000 + r % (ram_vars_end - 0x1000));
	default: return r & 0x3FFF;
	}
}

/* Run until the function returns (PC = ret with SP = sp_after); false on a timeout or halt. */
static bool run_to_return(mcu_t *m, uint16_t ret, uint16_t sp_after)
{
	static uint8_t bp[8192];
	memset(bp, 0, sizeof bp);
	bp[ret >> 3] |= (uint8_t)(1 << (ret & 7));
	m->breakpoints = bp;
	uint64_t end = m->now + mcu_ticks(m, limit_ms / 1000);
	for (;;) {
		mcu_run(m, end);
		if (m->cpu.state == CPU12_HALT || m->now >= end)
			return false;
		if (m->cpu.pc == ret && m->cpu.sp == sp_after)
			return true;
		if (!m->stop)
			return false;
	}
}

static const char *ram_label(uint16_t a, char *buf, size_t n)
{
	if (a >= 0x1000 && a < 0x2000) {
		for (int back = 0; back < 64 && a - back >= 0x1000; back++) {
			const char *s = ram_name[a - back - 0x1000];
			if (s) {
				if (back)
					snprintf(buf, n, "%s+%d", s, back);
				else
					snprintf(buf, n, "%s", s);
				return buf;
			}
		}
	}
	snprintf(buf, n, "-");
	return buf;
}

/* The byte at CPU address a of flash pages fl, page $3D in the window */
static uint8_t flash_byte(uint8_t fl[FLASH_PAGES][PAGE_SIZE], uint16_t a)
{
	return a >= 0xC000 ? fl[3][a - 0xC000] : a >= 0x8000 ? fl[1][a - 0x8000] : fl[2][a - 0x4000];
}

/* Whether a pointer argument of f (D, then f->flash_args stack argument bytes) reaches a string
 * in flash, up to its 0, whose bytes the rebuild's image does not have at that address: the
 * original's read-only data the rebuild keeps elsewhere. Such a call is not tested. */
static bool flash_arg_differs(func_t *f, mcu_t *m)
{
	uint16_t sp = m->cpu.sp;
	for (int i = -2; i < f->flash_args; i += 2) {
		uint16_t v = i < 0 ? cpu12_d(&m->cpu) : rd16(m, (uint16_t)(sp + 2 + i));
		for (unsigned k = 0; v >= 0x4000 && v + k < 0xF800 && k < 64; k++) {
			uint8_t o = flash_byte(m->flash, (uint16_t)(v + k));
			if (o != flash_byte(rebuild_flash, (uint16_t)(v + k)))
				return true;
			if (!o)
				break;
		}
	}
	return false;
}

/* Complete the EEPROM commands under way: a sector erase takes milliseconds, so one started
 * before the call can finish inside one side's call and after the other's. */
static void eeprom_settle(mcu_t *m)
{
	while (m->ee.has_active)
		eeprom_event(m);
}

static void test_call(func_t *f, mcu_t *m, ms1_board_t *b)
{
	static mcu_t ma, mb;
	static ms1_board_t ba, bb;
	static iolog_t la, lb;
	uint16_t sp0 = m->cpu.sp, ret = rd16(m, sp0);

	clone(&ma, &ba, m, b);
	clone(&mb, &bb, m, b);
	memcpy(mb.flash, rebuild_flash, sizeof rebuild_flash);
	mb.cpu.pc = f->rebuild;
	la.n = lb.n = 0;
	la.overflow = lb.overflow = false;
	ma.irq_block = mb.irq_block = true;
	ma.write_hook = mb.write_hook = io_hook;
	ma.hook_ctx = &la;
	mb.hook_ctx = &lb;

	unsigned trace_start = mb.trace_pos, trace_start_a = ma.trace_pos;
	double t0 = mcu_seconds(&ma, ma.now);
	bool okA = run_to_return(&ma, ret, (uint16_t)(sp0 + 2));
	bool okB = run_to_return(&mb, ret, (uint16_t)(sp0 + 2));
	f->tested++;
	if (okA && okB) {
		f->us_orig += (mcu_seconds(&ma, ma.now) - t0) * 1e6;
		f->us_rebuild += (mcu_seconds(&mb, mb.now) - t0) * 1e6;
		f->timed++;
	}
	if (!okA) {
		f->timeout++;
		return;
	}
	if (!okB) {
		f->rebuild_timeout++;
		f->differ++;
		if (!f->reported || verbose) {
			printf("%-36s call %u: the rebuild does not return (PC %04X after %.0f ms; original returned)\n",
				f->name, f->calls, mb.cpu.pc, limit_ms);
			f->reported = true;
		}
		return;
	}

	/* compare */
	static char msg[65536];
	int n = 0, diffs = 0;
	msg[0] = 0;
	if (f->ret == 'w' && (ma.cpu.a != mb.cpu.a || ma.cpu.b != mb.cpu.b))
		n += snprintf(msg + n, sizeof msg - (size_t)n, "  return D: original %04X, rebuild %04X\n",
			cpu12_d(&ma.cpu), cpu12_d(&mb.cpu)), diffs++;
	if (f->ret == 'b' && ma.cpu.b != mb.cpu.b)
		n += snprintf(msg + n, sizeof msg - (size_t)n, "  return B: original %02X, rebuild %02X\n",
			ma.cpu.b, mb.cpu.b), diffs++;
	int shown = 0;
	for (unsigned a = 0x1000; a < 0x2000; a++) {
		if (a >= ram_vars_end && a < (unsigned)sp0 + 2 + f->argbytes)
			continue;
		uint8_t va = ma.ram[a - 0x1000], vb = mb.ram[a - 0x1000];
		if (va == vb || past_frame_length(&ma, a))
			continue;
		diffs++;
		if (shown++ < 12 || verbose) {
			char lab[96];
			n += snprintf(msg + n, sizeof msg - (size_t)n, "  RAM %04X %-28s original %02X rebuild %02X (before %02X)\n",
				a, ram_label((uint16_t)a, lab, sizeof lab), va, vb, m->ram[a - 0x1000]);
		}
	}
	if (shown > 12 && !verbose)
		n += snprintf(msg + n, sizeof msg - (size_t)n, "  ... %d RAM bytes differ\n", shown);
	eeprom_settle(&ma);
	eeprom_settle(&mb);
	for (unsigned i = 0; i < EEPROM_SIZE; i++)
		if (ma.eeprom[i] != mb.eeprom[i]) {
			n += snprintf(msg + n, sizeof msg - (size_t)n, "  EEPROM %04X original %02X rebuild %02X\n",
				0x800 + i, ma.eeprom[i], mb.eeprom[i]);
			diffs++;
			break;
		}
	int k = 0;
	while (k < la.n && k < lb.n && la.addr[k] == lb.addr[k] && la.val[k] == lb.val[k])
		k++;
	if (k < la.n || k < lb.n || la.overflow != lb.overflow) {
		diffs++;
		n += snprintf(msg + n, sizeof msg - (size_t)n, "  I/O writes: %d and %d; first difference at #%d:", la.n, lb.n, k);
		if (k < la.n)
			n += snprintf(msg + n, sizeof msg - (size_t)n, " original $%03X=%02X", la.addr[k], la.val[k]);
		else
			n += snprintf(msg + n, sizeof msg - (size_t)n, " original none");
		if (k < lb.n)
			n += snprintf(msg + n, sizeof msg - (size_t)n, ", rebuild $%03X=%02X\n", lb.addr[k], lb.val[k]);
		else
			n += snprintf(msg + n, sizeof msg - (size_t)n, ", rebuild none\n");
	}
	if (!diffs) {
		f->ok++;
		return;
	}
	f->differ++;
	if (show_path && (!f->reported || verbose)) {
		uint16_t end = 0xFFFF;
		for (int i = 0; i < nsyms; i++)
			if (syms[i].addr > f->rebuild && syms[i].addr < end)
				end = syms[i].addr;
		unsigned from = mb.trace_pos - trace_start > 4096 ? mb.trace_pos - 4096 : trace_start, col = 0;
		n += snprintf(msg + n, sizeof msg - (size_t)n, "  rebuild path in %s:", f->name);
		for (unsigned i = from; i < mb.trace_pos && n < (int)sizeof msg - 16; i++) {
			uint16_t pc = mb.trace[i & 4095];
			if (pc >= f->rebuild && pc < end)
				n += snprintf(msg + n, sizeof msg - (size_t)n, "%s%04X", col++ % 16 ? " " : "\n   ", pc);
		}
		n += snprintf(msg + n, sizeof msg - (size_t)n, "\n");
		/* the original's path: up to the next listed function's entry */
		end = 0xFFFF;
		for (int i = 0; i < nfuncs; i++)
			if (funcs[i].orig > f->orig && funcs[i].orig < end)
				end = funcs[i].orig;
		from = ma.trace_pos - trace_start_a > 4096 ? ma.trace_pos - 4096 : trace_start_a;
		col = 0;
		n += snprintf(msg + n, sizeof msg - (size_t)n, "  original path:");
		for (unsigned i = from; i < ma.trace_pos && n < (int)sizeof msg - 16; i++) {
			uint16_t pc = ma.trace[i & 4095];
			if (pc >= f->orig && pc < end)
				n += snprintf(msg + n, sizeof msg - (size_t)n, "%s%04X", col++ % 16 ? " " : "\n   ", pc);
		}
		n += snprintf(msg + n, sizeof msg - (size_t)n, "\n");
	}
	if (!f->reported || verbose) {
		printf("%-36s call %u at t=%.6fs (D=%04X X=%04X Y=%04X SP=%04X, caller %04X):\n%s", f->name, f->calls,
			mcu_seconds(m, m->now), cpu12_d(&m->cpu), m->cpu.x, m->cpu.y, sp0, ret, msg);
		f->reported = true;
	}
}

static int button_index(const char *s)
{
	static const struct { const char *name; int index; } names[] = {
		{ "esc", 8 }, { "ok", 9 }, { "light", 10 }, { "stop", 11 }, { "knob", 12 },
	};
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
		if (!strcmp(s, names[i].name))
			return names[i].index;
	if (s[0] == 'p' && (s[1] == 'a' || s[1] == 'b') && s[2] >= '0' && s[2] <= '7' && !s[3]) {
		int i = (s[1] == 'a' ? 0 : 8) + (s[2] - '0');
		return i;
	}
	return -1;
}

static mcu_t mcu;
static ms1_board_t board;

/* The second MS1 (-M): its machine and its scenario inputs */
static mcu_t peer_mcu;
static ms1_board_t peer_board;
static bool have_peer;
#define MAXEV 4096
static struct { double t; char act[16]; char arg[16]; } peer_ev[MAXEV];
static int peer_nev, peer_next;
#define LOCKSTEP 0.0001

/* The instruction just executed was a JSR, BSR or CALL (not a branch back to a loop at a
 * function's first instruction). */
static bool entered_by_call(mcu_t *m)
{
	uint16_t pc = m->trace[(m->trace_pos - 1) & 4095];
	uint8_t op = mcu_peek(m, pc);
	return op == 0x15 || op == 0x16 || op == 0x17 || op == 0x07 || op == 0x4A || op == 0x4B;
}
static uint8_t func_bp[8192];

static void test_call(func_t *f, mcu_t *m, ms1_board_t *b);

/* Call f count times from the original's present state, as a JSR would. */
static void call_directly(func_t *f, int count, mcu_t *src, ms1_board_t *bsrc)
{
	static mcu_t mc;
	static ms1_board_t bc;
	for (int k = 0; k < count; k++) {
		clone(&mc, &bc, src, bsrc);
		uint16_t sp = (uint16_t)(mc.cpu.sp - f->argbytes);
		for (unsigned i = 0; i < f->argbytes; i += 2) {
			uint16_t w = input_word(k + i / 2 + 3);
			mc.ram[(uint16_t)(sp + i) - 0x1000] = (uint8_t)(w >> 8);
			if (i + 1 < f->argbytes)
				mc.ram[(uint16_t)(sp + i + 1) - 0x1000] = (uint8_t)w;
		}
		sp = (uint16_t)(sp - 2);
		mc.ram[sp - 0x1000] = DIRECT_RETURN >> 8;
		mc.ram[sp + 1 - 0x1000] = DIRECT_RETURN & 0xFF;
		mc.cpu.sp = sp;
		mc.cpu.pc = f->orig;
		uint16_t d = input_word(k);
		mc.cpu.a = (uint8_t)(d >> 8);
		mc.cpu.b = (uint8_t)d;
		mc.cpu.x = input_word(k + 1);
		mc.cpu.y = input_word(k + 2);
		f->direct++;
		test_call(f, &mc, &bc);
	}
}

static void apply_input(ms1_board_t *b, const char *act, const char *arg);

/* Run the second MS1 to a time, applying its inputs on the way. */
static void run_peer(uint64_t until)
{
	while (peer_mcu.now < until) {
		uint64_t to = until;
		if (peer_next < peer_nev) {
			uint64_t te = mcu_ticks(&peer_mcu, peer_ev[peer_next].t);
			if (te <= peer_mcu.now) {
				apply_input(&peer_board, peer_ev[peer_next].act, peer_ev[peer_next].arg);
				peer_next++;
				continue;
			}
			if (te < to)
				to = te;
		}
		mcu_run(&peer_mcu, to);
		if (peer_mcu.cpu.state == CPU12_HALT) {
			fprintf(stderr, "second MS1 halted at %04X\n", peer_mcu.cpu.pc);
			exit(1);
		}
	}
}

/* Run the original to a time, testing every listed function it enters. */
static void run_until(uint64_t until)
{
	mcu_t *m = &mcu;
	while (m->now < until) {
		uint64_t to = until;
		if (have_peer) {
			/* lockstep: the second MS1 runs first to the end of the slice */
			uint64_t slice = m->now + mcu_ticks(m, LOCKSTEP);
			if (slice < to)
				to = slice;
			run_peer(to);
		}
		m->breakpoints = func_bp;
		mcu_run(m, to);
		if (m->cpu.state == CPU12_HALT) {
			fprintf(stderr, "original halted at %04X: %s\n", m->cpu.pc, m->stop_reason ? m->stop_reason : "?");
			exit(1);
		}
		if (!m->stop)
			continue;
		func_t *f = by_addr[m->cpu.pc];
		if (f && f->enabled && entered_by_call(m)) {
			f->calls++;
			if (f->tested < (unsigned)max_tests) {
				if (f->flash_args >= 0 && flash_arg_differs(f, m))
					f->skipped++;
				else
					test_call(f, m, &board);
			}
		}
		/* mcu_run steps past the breakpoint it stopped at when called again */
	}
}

static void apply_input(ms1_board_t *b, const char *act, const char *arg)
{
	if (!strcmp(act, "press") || !strcmp(act, "release"))
		board_button(b, button_index(arg), act[0] == 'p');
	else if (!strcmp(act, "turn"))
		board_encoder_step(b, atoi(arg));
	else if (!strcmp(act, "unplug") || !strcmp(act, "plug"))
		board_can_plug(b, act[0] == 'p');
}

/* Read a scenario file's input lines; 1 on an error. */
static int read_script(const char *path, void (*line_fn)(double, const char *, const char *))
{
	FILE *f = fopen(path, "r");
	char line[256], act[32], arg[32];
	double t;
	if (!f) {
		perror(path);
		return 1;
	}
	while (fgets(line, sizeof line, f)) {
		arg[0] = 0;
		if (line[0] == '#' || sscanf(line, "%lf %31s %31s", &t, act, arg) < 2)
			continue;
		if ((!strcmp(act, "press") || !strcmp(act, "release")) && button_index(arg) < 0) {
			fprintf(stderr, "%s: unknown button '%s'\n", path, arg);
			fclose(f);
			return 1;
		}
		line_fn(t, act, arg);
	}
	fclose(f);
	return 0;
}

static double script_end;

static void own_line(double t, const char *act, const char *arg)
{
	if (t > script_end)
		return;
	run_until(mcu_ticks(&mcu, t));
	if (!strcmp(act, "text")) {
		char text[64];
		board_lcd_text(&board, text, sizeof text);
		printf("t=%.3fs \"%s\"\n", t, text);
	} else {
		apply_input(&board, act, arg);
	}
}

static void peer_line(double t, const char *act, const char *arg)
{
	if (peer_nev == MAXEV)
		return;
	peer_ev[peer_nev].t = t;
	snprintf(peer_ev[peer_nev].act, sizeof peer_ev[0].act, "%s", act);
	snprintf(peer_ev[peer_nev].arg, sizeof peer_ev[0].arg, "%s", arg);
	peer_nev++;
}

static int run_script(const char *path, double seconds)
{
	script_end = seconds;
	return read_script(path, own_line);
}

static int load_symbols(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[256];
	if (!f) {
		perror(path);
		return 1;
	}
	while (fgets(line, sizeof line, f)) {
		unsigned a;
		char type[8], name[64];
		if (sscanf(line, "%x %7s %63s", &a, type, name) != 3 || a > 0xFFFF || name[0] == '.')
			continue;
		if (a >= 0x1000 && a < 0x2000) {
			if (!ram_name[a - 0x1000] && strcmp(name, "__data_section_start") && strcmp(name, "__bss_start"))
				ram_name[a - 0x1000] = strdup(name);
		} else if (type[0] == 'T' || type[0] == 't') {
			syms = realloc(syms, (size_t)(nsyms + 1) * sizeof *syms);
			snprintf(syms[nsyms].name, sizeof syms[nsyms].name, "%s", name);
			syms[nsyms++].addr = (uint16_t)a;
		}
	}
	fclose(f);
	return 0;
}

static int load_functions(const char *path, const char **only, int nonly)
{
	FILE *f = fopen(path, "r");
	char line[256];
	if (!f) {
		perror(path);
		return 1;
	}
	while (fgets(line, sizeof line, f)) {
		unsigned o;
		unsigned argbytes = 0;
		char name[64], ret[8] = "v", flags[8] = "";
		if (line[0] == '#' || sscanf(line, "%x %63s %7s %u %7s", &o, name, ret, &argbytes, flags) < 2)
			continue;
		if (nfuncs == MAXF || o > 0xFFFF)
			continue;
		int s = 0;
		while (s < nsyms && strcmp(syms[s].name, name))
			s++;
		if (s == nsyms) {
			/* not linked into the rebuild (nothing calls it there): not tested */
			fprintf(stderr, "%s: %s is not in the rebuild's symbols, not tested\n", path, name);
			continue;
		}
		func_t *fn = &funcs[nfuncs++];
		fn->orig = (uint16_t)o;
		fn->rebuild = syms[s].addr;
		snprintf(fn->name, sizeof fn->name, "%s", name);
		fn->ret = ret[0];
		fn->argbytes = argbytes;
		fn->flash_args = flags[0] == 'p' ? atoi(flags + 1) : -1;
		fn->enabled = !nonly;
		for (int i = 0; i < nonly; i++)
			if (!strcmp(only[i], name))
				fn->enabled = true;
		by_addr[o] = fn;
		if (fn->enabled)
			func_bp[o >> 3] |= (uint8_t)(1 << (o & 7));
	}
	fclose(f);
	return 0;
}

static void usage(void)
{
	fprintf(stderr, "usage: difftest [-t SECONDS] [-s SCRIPT] [-a CH=V] [-e EEPROM] [-n N] [-m MS] [-v] [-f NAME]...\n"
			"                [-r ADDR:COUNT]... [-M IMAGE [-S SCRIPT] [-A CH=V]...] [-D SPEC]... [-u NAME[:N]]...\n"
			"                [-c FILE] [-W] [-y] [-B SECONDS]\n"
			"                ORIGINAL.s19 REBUILD.s19 SYMBOLS FUNCTIONS\n");
	exit(2);
}

int main(int argc, char **argv)
{
	double seconds = 15;
	const char *script = 0, *eeprom = 0, *peer_image = 0, *peer_script = 0;
	const char *only[64], *decoder_spec[8], *can_log_path = 0;
	bool whole = false, irq_timing = false;
	double hold = 0;
	int nonly = 0, ndecoder_specs = 0, opt;
	int atd_set[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
	int peer_atd[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
	while ((opt = getopt(argc, argv, "t:s:a:e:n:m:vxf:M:S:r:A:D:u:c:WyIB:")) != -1) {
		switch (opt) {
		case 't': seconds = atof(optarg); break;
		case 's': script = optarg; break;
		case 'e': eeprom = optarg; break;
		case 'n': max_tests = atoi(optarg); break;
		case 'm': limit_ms = atof(optarg); break;
		case 'v': verbose = true; break;
		case 'x': show_path = true; break;
		case 'f': if (nonly < 64) only[nonly++] = optarg; break;
		case 'M': peer_image = optarg; break;
		case 'D': if (ndecoder_specs < 8) decoder_spec[ndecoder_specs++] = optarg; break;
		case 'c': can_log_path = optarg; break;
		case 'W': whole = true; break;
		case 'I': irq_timing = true; break;
		case 'y': show_times = true; break;
		case 'B': hold = atof(optarg); break;
		case 'u': {
			if (ndirect == 32)
				usage();
			char *colon = strchr(optarg, ':');
			direct[ndirect].count = colon ? atoi(colon + 1) : 50;
			if (colon)
				*colon = 0;
			direct[ndirect++].name = optarg;
			break;
		}
		case 'r': {
			unsigned a, n;
			if (nrings == 4 || sscanf(optarg, "%x:%u", &a, &n) != 2 || a < 0x1000 || a + n * FRAME_SIZE > 0x2000)
				usage();
			rings[nrings].addr = (uint16_t)a;
			rings[nrings++].count = (uint16_t)n;
			break;
		}
		case 'S': peer_script = optarg; break;
		case 'a':
		case 'A': {
			int ch;
			unsigned v;
			if (sscanf(optarg, "%d=%u", &ch, &v) != 2 || ch < 0 || ch > 7 || v > 1023)
				usage();
			(opt == 'a' ? atd_set : peer_atd)[ch] = (int)v;
			break;
		}
		default: usage();
		}
	}
	if (optind != argc - 4)
		usage();
	char err[256];
	mcu_init(&mcu, 4000000, 0);
	if (image_load_s19(argv[optind], mcu.flash, err, sizeof err) ||
	    image_load_s19(argv[optind + 1], rebuild_flash, err, sizeof err)) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	if (mcu_peek(&mcu, 0xF73C) == 0x40 && mcu_peek(&mcu, 0xF73D) == 0x10)
		ram_vars_end = 0x1BAC;
	if (eeprom) {
		FILE *f = fopen(eeprom, "rb");
		if (!f || fread(mcu.eeprom, 1, EEPROM_SIZE, f) != EEPROM_SIZE) {
			fprintf(stderr, "%s: cannot read 1024 bytes\n", eeprom);
			return 1;
		}
		fclose(f);
	}
	if (load_symbols(argv[optind + 2]) || load_functions(argv[optind + 3], only, nonly))
		return 1;
	if (whole) {
		memcpy(mcu.flash, rebuild_flash, sizeof rebuild_flash);
		for (int i = 0; i < nfuncs; i++)
			funcs[i].enabled = false;
		memset(func_bp, 0, sizeof func_bp);
	}
	FILE *can_log = 0;
	if (can_log_path && !(can_log = fopen(can_log_path, "w"))) {
		perror(can_log_path);
		return 1;
	}
	board_init(&board, &mcu, can_log);
	if (whole && board_lcd_symbols(&board, argv[optind + 2]))
		return 1;
	if (irq_timing && whole)
		irqstat_attach(&mcu);
	for (int i = 0; i < 8; i++)
		if (atd_set[i] >= 0)
			board.atd[i] = (uint16_t)atd_set[i];
	for (int i = 0; i < ndecoder_specs; i++) {
		/* on the original's track; the copies that run a tested call have none */
		decoder_feedback_t fb = board_feedback(&board);
		decoder_t *d = decoder_create(decoder_spec[i], stdout, &fb, err, sizeof err);
		if (!d) {
			fprintf(stderr, "%s\n", err);
			return 1;
		}
		board_attach_decoder(&board, d);
	}
	mcu_reset(&mcu);
	if (hold > 0)
		mcu_hold_start(&mcu, hold);
	if (peer_image) {
		mcu_init(&peer_mcu, 4000000, 0);
		if (image_load_s19(peer_image, peer_mcu.flash, err, sizeof err)) {
			fprintf(stderr, "%s\n", err);
			return 1;
		}
		board_init(&peer_board, &peer_mcu, 0);
		for (int i = 0; i < 8; i++)
			if (peer_atd[i] >= 0)
				peer_board.atd[i] = (uint16_t)peer_atd[i];
		mcu_reset(&peer_mcu);
		if (hold > 0)
			mcu_hold_start(&peer_mcu, hold);
		board.peer = &peer_board;
		peer_board.peer = &board;
		have_peer = true;
		if (peer_script && read_script(peer_script, peer_line))
			return 1;
	}
	if (script && run_script(script, seconds))
		return 1;
	run_until(mcu_ticks(&mcu, seconds));
	for (int i = 0; i < ndirect && !whole; i++) {
		int k = 0;
		while (k < nfuncs && strcmp(funcs[k].name, direct[i].name))
			k++;
		if (k == nfuncs || !funcs[k].enabled) {
			fprintf(stderr, "-u %s: not a tested function of the table\n", direct[i].name);
			return 1;
		}
		call_directly(&funcs[k], direct[i].count, &mcu, &board);
	}
	char text[64];
	board_lcd_text(&board, text, sizeof text);
	printf("%s at t=%.3fs: PC %04X, LCD \"%s\"\n", whole ? "rebuild" : "original", mcu_seconds(&mcu, mcu.now),
		mcu.cpu.pc, text);
	if (irq_timing && whole)
		irqstat_print(&mcu, stdout);

	printf("\n%-36s %5s %5s %6s %5s %6s %7s %7s\n", "function", "orig", "build", "calls", "tests", "match", "differ",
	       "skipped");
	unsigned tot_ok = 0, tot_diff = 0, tot_skip = 0;
	for (int i = 0; i < nfuncs; i++) {
		func_t *f = &funcs[i];
		if (!f->enabled)
			continue;
		printf("%-36s %04X  %04X %6u %5u %6u %7u %7u%s%s\n", f->name, f->orig, f->rebuild, f->calls, f->tested, f->ok,
			f->differ, f->skipped, f->timeout ? "  (original timed out on some calls)" : "",
			f->direct ? "  (called directly)" : "");
		tot_ok += f->ok;
		tot_diff += f->differ;
		tot_skip += f->skipped;
	}
	printf("total: %u calls match, %u differ, %u skipped\n", tot_ok, tot_diff, tot_skip);
	if (show_times) {
		printf("\n%-36s %6s %12s %12s %7s\n", "function", "timed", "orig us", "rebuild us", "ratio");
		for (int i = 0; i < nfuncs; i++) {
			func_t *f = &funcs[i];
			if (!f->enabled || !f->timed)
				continue;
			printf("%-36s %6u %12.1f %12.1f %7.2f\n", f->name, f->timed, f->us_orig / f->timed,
				f->us_rebuild / f->timed, f->us_orig > 0 ? f->us_rebuild / f->us_orig : 0);
		}
	}
	return tot_diff ? 1 : 0;
}
