/* ms1emu: run the MS1 firmware on the MC9S12D64 model.
 *   ms1emu [options] <image.s19>
 * See README.md for the options. */
#define _POSIX_C_SOURCE 200809L
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "canbus.h"
#include "mcu.h"
#include "tui.h"

static mcu_t mcu;
static ms1_board_t board;

static uint8_t peek(void *ctx, uint16_t a) { return mcu_peek(ctx, a); }

static void print_state(mcu_t *m, FILE *out)
{
	cpu12_t *c = &m->cpu;
	char dis[64];
	cpu12_disasm(peek, m, c->pc, dis, sizeof dis);
	fprintf(out, "t=%.6fs PC=%04X %-24s A=%02X B=%02X X=%04X Y=%04X SP=%04X CCR=%02X PPAGE=%02X%s%s\n",
		mcu_seconds(m, m->now), c->pc, dis, c->a, c->b, c->x, c->y, c->sp, c->ccr, m->regs[R_PPAGE],
		c->state == CPU12_WAIT ? " WAIT" : c->state == CPU12_STOP ? " STOP" : "",
		c->state == CPU12_HALT ? " HALT" : "");
}

static void print_trace(mcu_t *m, FILE *out, int n)
{
	char dis[64];
	for (int i = n; i > 0; i--) {
		uint16_t pc = m->trace[(m->trace_pos - (unsigned)i) & 4095];
		cpu12_disasm(peek, m, pc, dis, sizeof dis);
		fprintf(out, "  %04X  %s\n", pc, dis);
	}
}

/* Where the CPU spends its time: sample the PC every few microseconds of emulated time. */
static void profile(mcu_t *m, double seconds, FILE *out)
{
	static uint32_t hits[65536];
	memset(hits, 0, sizeof hits);
	uint64_t step = mcu_ticks(m, 5e-6), end = m->now + mcu_ticks(m, seconds);
	unsigned long samples = 0;
	while (m->now < end) {
		mcu_run(m, m->now + step);
		hits[m->cpu.pc]++;
		samples++;
		if (m->stop)
			break;
	}
	fprintf(out, "profile over %.3fs (%lu samples):\n", seconds, samples);
	for (int k = 0; k < 15; k++) {
		uint32_t best = 0;
		int at = -1;
		for (int a = 0; a < 65536; a++)
			if (hits[a] > best) {
				best = hits[a];
				at = a;
			}
		if (at < 0)
			break;
		char dis[64];
		cpu12_disasm(peek, m, (uint16_t)at, dis, sizeof dis);
		fprintf(out, "  %5.1f%%  %04X  %s\n", 100.0 * best / samples, at, dis);
		hits[at] = 0;
	}
}

/* Button names: the MS1 controls on PORTB, as ui_task uses them (README: MS1
 * board model). */
static const struct { const char *name; int index; } buttons[] = {
	{ "esc", 8 }, { "ok", 9 }, { "light", 10 }, { "stop", 11 }, { "knob", 12 },
};

static int button_index(const char *s)
{
	for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; i++)
		if (!strcmp(s, buttons[i].name))
			return buttons[i].index;
	if (s[0] == 'p' && (s[1] == 'a' || s[1] == 'b') && s[2] >= '0' && s[2] <= '7' && !s[3]) {
		int i = (s[1] == 'a' ? 0 : 8) + (s[2] - '0');
		return i;
	}
	return -1;
}

/* Run to a tick; with realtime, no faster than the wall clock (for a shared bus). */
static void run_until(mcu_t *m, uint64_t until, bool realtime)
{
	if (!realtime) {
		mcu_run(m, until);
		return;
	}
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	double e0 = mcu_seconds(m, m->now);
	while (m->now < until && !m->stop) {
		uint64_t step = m->now + mcu_ticks(m, 0.002);
		mcu_run(m, step < until ? step : until);
		struct timespec t;
		clock_gettime(CLOCK_MONOTONIC, &t);
		double ahead = (mcu_seconds(m, m->now) - e0) - ((double)(t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) * 1e-9);
		if (ahead > 0) {
			struct timespec ts = { 0, (long)(ahead * 1e9) };
			nanosleep(&ts, 0);
		}
	}
}

static bool realtime_script;

/* Timed inputs: each line "SECONDS action ...", in time order. */
static int run_script(mcu_t *m, const char *path)
{
	FILE *f = fopen(path, "r");
	char line[256], act[32], arg[32];
	double t;
	if (!f) {
		perror(path);
		return 1;
	}
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '#' || sscanf(line, "%lf %31s %31s", &t, act, arg) < 2)
			continue;
		run_until(m, mcu_ticks(m, t), realtime_script);
		if (m->stop)
			break;
		if (!strcmp(act, "press") || !strcmp(act, "release")) {
			int idx = button_index(arg);
			if (idx < 0) {
				fprintf(stderr, "%s: unknown button '%s'\n", path, arg);
				return 1;
			}
			board_button(&board, idx, act[0] == 'p');
		} else if (!strcmp(act, "turn")) {
			board_encoder_step(&board, atoi(arg));
		} else if (!strcmp(act, "unplug") || !strcmp(act, "plug")) {
			board_can_plug(&board, act[0] == 'p');
		} else if (!strcmp(act, "lcd")) {
			printf("t=%.3fs ", mcu_seconds(m, m->now));
			board_lcd_print(&board, stdout);
		} else if (!strcmp(act, "text")) {
			char text[64];
			board_lcd_text(&board, text, sizeof text);
			printf("t=%.3fs \"%s\"\n", mcu_seconds(m, m->now), text);
		} else {
			fprintf(stderr, "%s: unknown action '%s'\n", path, act);
			return 1;
		}
	}
	fclose(f);
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: ms1emu [options] <image.s19>\n"
		"  -t SECONDS     emulated time to run (default 1)\n"
		"  -b ADDR        breakpoint (hex), repeatable\n"
		"  -p SECONDS     profile: where the PC is, sampled over this time after the run\n"
		"  -e FILE        EEPROM contents (1024 bytes); default erased\n"
		"  -n N           instructions of trace shown when stopped (default 24, max 4096)\n"
		"  -q             no diagnostics log on stderr\n"
		"  -d ADDR:LEN    dump memory (hex) after the run, repeatable\n"
		"  -l             print the LCD after the run\n"
		"  -c FILE        log CAN frames to FILE (- for stdout)\n"
		"  -i             interactive: real time, LCD in the terminal, keys drive the controls\n"
		"  -C BUS         attach a CAN bus: a SocketCAN interface (can0, vcan0) or udp:PORT\n"
		"  -r             run no faster than real time (for a shared bus)\n"
		"  -a CH=VALUE    ATD0 channel input, 10-bit (default 512); 0=0 starts the MS1 as a CAN slave\n"
		"  -T FILE        log track output changes: time, PT3, PT4, PT5\n"
		"  -D SPEC        attach a locomotive decoder to the track, repeatable; it logs what it does\n"
		"                 and an analysis of the signal at the end: mm:ADDR[,mm1][,packets],\n"
		"                 mfx:UID[,name=NAME][,symbol=elok|diesel|steam|none][,cv=VVV/II:XX]...\n"
		"                 [,noread][,frames]\n"
		"  -s FILE        input script: lines 'SECONDS press|release BUTTON', 'SECONDS turn N',\n"
		"                 'SECONDS lcd', 'SECONDS text', 'SECONDS unplug|plug' (the CAN cable);\n"
		"                 BUTTON is pa0-pa7, pb0-pb7 or ok, esc, knob, stop, light\n");
	exit(2);
}

int main(int argc, char **argv)
{
	double seconds = 1, prof = 0;
	int trace_len = 24;
	unsigned dumps[16][2];
	int ndumps = 0;
	const char *eeprom = 0;
	bool quiet = false, show_lcd = false, interactive = false, realtime = false;
	const char *bus_spec = 0;
	int atd_set[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
	static canbus_t bus;
	FILE *can_log = 0, *track_log = 0;
	const char *script = 0;
	const char *decoder_spec[8];
	int ndecoder_specs = 0;
	static uint8_t bp[8192];
	bool have_bp = false;
	int opt;
	while ((opt = getopt(argc, argv, "t:b:p:e:qn:d:lc:s:T:iC:ra:D:")) != -1) {
		switch (opt) {
		case 't': seconds = atof(optarg); break;
		case 'b': {
			unsigned a = (unsigned)strtoul(optarg, 0, 16) & 0xFFFF;
			bp[a >> 3] |= (uint8_t)(1 << (a & 7));
			have_bp = true;
			break;
		}
		case 'p': prof = atof(optarg); break;
		case 'e': eeprom = optarg; break;
		case 'q': quiet = true; break;
		case 'd':
			if (ndumps < 16 && sscanf(optarg, "%x:%x", &dumps[ndumps][0], &dumps[ndumps][1]) == 2)
				ndumps++;
			break;
		case 'l': show_lcd = true; break;
		case 'c': can_log = strcmp(optarg, "-") ? fopen(optarg, "w") : stdout; break;
		case 's': script = optarg; break;
		case 'i': interactive = true; break;
		case 'C': bus_spec = optarg; break;
		case 'r': realtime = true; break;
		case 'a': {
			int ch;
			unsigned v;
			if (sscanf(optarg, "%d=%u", &ch, &v) != 2 || ch < 0 || ch > 7 || v > 1023)
				usage();
			atd_set[ch] = (int)v;
			break;
		}
		case 'T': track_log = fopen(optarg, "w"); break;
		case 'D':
			if (ndecoder_specs < 8)
				decoder_spec[ndecoder_specs++] = optarg;
			break;
		case 'n': trace_len = atoi(optarg) > 4096 ? 4096 : atoi(optarg); break;
		default: usage();
		}
	}
	if (optind != argc - 1)
		usage();

	mcu_t *m = &mcu;
	mcu_init(m, 4000000, quiet ? 0 : stderr);
	char err[256];
	if (image_load_s19(argv[optind], m->flash, err, sizeof err)) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	if (eeprom) {
		FILE *f = fopen(eeprom, "rb");
		if (!f || fread(m->eeprom, 1, EEPROM_SIZE, f) != EEPROM_SIZE) {
			fprintf(stderr, "%s: cannot read 1024 bytes\n", eeprom);
			return 1;
		}
		fclose(f);
	}
	if (have_bp)
		m->breakpoints = bp;
	board_init(&board, m, can_log);
	board.track_log = track_log;
	for (int i = 0; i < ndecoder_specs; i++) {
		/* interactive: the state shows on the screen, the analysis after quitting */
		decoder_feedback_t fb = board_feedback(&board);
		decoder_t *d = decoder_create(decoder_spec[i], interactive ? NULL : stdout, &fb, err, sizeof err);
		if (!d) {
			fprintf(stderr, "%s\n", err);
			return 1;
		}
		board_attach_decoder(&board, d);
	}
	for (int i = 0; i < 8; i++)
		if (atd_set[i] >= 0)
			board.atd[i] = (uint16_t)atd_set[i];
	if (bus_spec && canbus_open(&bus, bus_spec, err, sizeof err)) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	mcu_reset(m);   /* clears scheduled events: attach the bus after it */
	if (bus_spec)
		board_attach_bus(&board, &bus);
	if (interactive) {
		if (m->log == stderr)
			m->log = 0;   /* diagnostics would scribble over the screen */
		int rc = tui_run(m, &board);
		for (int i = 0; i < board.ndecoders; i++)
			decoder_report(board.decoders[i], mcu_seconds(m, m->now), stdout);
		return rc;
	}
	realtime_script = realtime;
	if (script && run_script(m, script))
		return 1;
	if (!m->stop)
		run_until(m, mcu_ticks(m, seconds), realtime);
	if (m->stop) {
		printf("stopped: %s\n", m->stop_reason ? m->stop_reason : "?");
		print_trace(m, stdout, trace_len);
	}
	print_state(m, stdout);
	printf("board: track edges PT3 %lu PT4 %lu PT5 %lu, CAN tx %lu rx %lu, LCD blocks %u\n",
		board.track_edges[0], board.track_edges[1], board.track_edges[2], board.can_tx, board.can_rx,
		board.lcd_frames);
	if (show_lcd)
		board_lcd_print(&board, stdout);
	for (int i = 0; i < board.ndecoders; i++)
		decoder_report(board.decoders[i], mcu_seconds(m, m->now), stdout);
	for (int d = 0; d < ndumps; d++)
		for (unsigned i = 0; i < dumps[d][1]; i += 16) {
			printf("%04X:", (dumps[d][0] + i) & 0xFFFF);
			for (unsigned j = i; j < i + 16 && j < dumps[d][1]; j++)
				printf(" %02X", mcu_peek(m, (uint16_t)(dumps[d][0] + j)));
			printf("\n");
		}
	if (prof > 0 && !m->stop)
		profile(m, prof, stdout);
	return 0;
}
