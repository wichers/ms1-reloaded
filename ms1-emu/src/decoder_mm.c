/* The Märklin Motorola locomotive decoder (mm:ADDR).
 *
 * Märklin Motorola (A. Scorzoni, "The manual of the new Märklin-Motorola format" v2.14):
 * - A bit is a 208 us cell (104 us for solenoid decoders) starting with a positive pulse:
 *   7/8 of the cell for a 1, 1/8 for a 0. A packet is 18 bits; a pause of at least a few
 *   cells separates packets.
 * - Bit pairs form 9 trits: 4 address trits (00 = 0, 11 = 1, 10 = open; the first trit is the
 *   least significant base-3 digit, open = 2; 0000 is address 80, all open the idle packet),
 *   the function trit (F0: 11 on, 00 off), then the data pairs A E B F C G D H.
 * - A decoder takes a packet when it is repeated after a short pause (t1; the MC145027
 *   decoder chip's dead-time discriminator allows about 1.74 ms).
 * - ABCD (A least significant) is the speed code: 0 stop, 1 reverse (MM1) or stop (MM2),
 *   2-15 speed level 1-14.
 * - EFGH = ABCD is the old format (MM1). Otherwise (MM2) EFGH is 1010 / 1011 reverse
 *   (speed 7-14 / 0-6), 0101 / 0100 forward (0-6 / 7-14); or a function: EFG 110 f1, 001 f2,
 *   011 f3, 111 f4 with H its state, where 1010 at speed codes 3, 4, 6, 7 means f1-f4 off and
 *   0101 at speed codes 11, 12, 14, 15 f1-f4 on (the codes that would read as MM1).
 * An MM1 decoder looks only at A B C D (the first pulse of each pair) and toggles its
 * direction on a reverse packet. The MM2 decoder modelled here takes the absolute direction
 * from MM2 packets; a reverse packet stops it and, before any MM2 packet was seen, toggles
 * the direction. */
#include "decoder_int.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MM_BITS 18
#define MM_LOCO_CELL_US 208.0
#define MM_SOLENOID_CELL_US 104.0
#define MM_PAUSE_US 400.0         /* a negative level this long ends a packet */
#define MM_PULSE_MAX_US 300.0     /* a longer positive level is no MM pulse */
#define MM_REPEAT_US 2500.0       /* the repeat must start within this after the packet */
#define MC145027_DEAD_TIME_US 1742.0
#define MM_SEQUENCE_US 100000.0   /* gaps longer than this are no pause within MM traffic */
#define POWER_OFF_US 1000.0       /* 0 V this long is the track switched off (STOP) */

typedef struct {
	decoder_t *dec;
	bool mm1, log_packets;
	int address;
	/* edges */
	int pol;
	double t_last;
	bool in_pulse;
	/* the packet being received */
	int nbits;
	bool overflow;
	double rise[MM_BITS], high_us[MM_BITS];
	/* the last packet, waiting for its repeat */
	bool have_prev;
	uint8_t prev[MM_BITS];
	bool prev_solenoid;
	double prev_end, pair_end;
	/* the decoder */
	int speed;               /* 0-14 */
	bool reverse, f[5];      /* f[0] = F0 (the function trit), f[1-4] = f1-f4 */
	bool seen_mm2, last_was_reverse;
	double last_command;
	/* analysis */
	unsigned long packets, loco, solenoid, pairs, unpaired, bad_bits, other, idle, unknown;
	unsigned long t1_over, commands, per_address[81];
	stat_t cell, high1, high0, t1, t2, refresh, off;
} mm_t;

static const decoder_ops_t mm_ops;

bool mm_create(decoder_t *d, const char *args, char *err, size_t errsize)
{
	char *end;
	long a = strtol(args, &end, 10);
	if (end == args || a < 1 || a > 80) {
		snprintf(err, errsize, "decoder 'mm:%s': the MM address is 1-80", args);
		return false;
	}
	mm_t *m = calloc(1, sizeof *m);
	m->address = (int)a;
	while (*end == ',') {
		const char *opt = end + 1;
		size_t n = strcspn(opt, ",");
		if (n == 3 && !strncmp(opt, "mm1", 3)) {
			m->mm1 = true;
		} else if (n == 7 && !strncmp(opt, "packets", 7)) {
			m->log_packets = true;
		} else {
			snprintf(err, errsize, "decoder 'mm:%s': unknown option '%.*s'", args, (int)n, opt);
			free(m);
			return false;
		}
		end = (char *)opt + n;
	}
	if (*end) {
		snprintf(err, errsize, "decoder 'mm:%s': unexpected '%s'", args, end);
		free(m);
		return false;
	}
	snprintf(d->name, sizeof d->name, "%s:%d", m->mm1 ? "mm1" : "mm", m->address);
	m->dec = d;
	m->last_command = -1;
	m->pair_end = -1;
	d->ops = &mm_ops;
	d->impl = m;
	return true;
}

static void mm_free(decoder_t *d)
{
	free(d->impl);
}

static void set_speed(mm_t *d, double t, int level, int reverse)
{
	bool rev = reverse < 0 ? d->reverse : reverse > 0;
	if (level != d->speed || rev != d->reverse) {
		d->speed = level;
		d->reverse = rev;
		decoder_say(d->dec, t, "speed %s%d", rev ? "-" : "+", level);
	}
}

static void set_function(mm_t *d, double t, int i, bool on)
{
	if (d->f[i] != on) {
		d->f[i] = on;
		decoder_say(d->dec, t, "%s%d %s", i ? "f" : "F", i, on ? "on" : "off");
	}
}

/* A double packet for some locomotive address. */
static void command(mm_t *d, const uint8_t *b, double t)
{
	int numeric = 0, weight = 1;
	for (int k = 0; k < 4; k++, weight *= 3) {
		int pair = b[2 * k] << 1 | b[2 * k + 1];
		if (pair == 1) {       /* 01: no trit */
			d->unknown++;
			return;
		}
		numeric += (pair == 3 ? 1 : pair == 2 ? 2 : 0) * weight;
	}
	if (numeric == 80) {
		d->idle++;
		return;
	}
	int addr = numeric ? numeric : 80;
	d->per_address[addr]++;
	if (addr != d->address)
		return;

	d->commands++;
	if (d->last_command >= 0)
		stat_add(&d->refresh, (t - d->last_command) * 1e3);
	d->last_command = t;
	int A = b[10], E = b[11], B = b[12], F = b[13], C = b[14], G = b[15], D = b[16], H = b[17];
	int v = A | B << 1 | C << 2 | D << 3;
	int efgh = E << 3 | F << 2 | G << 1 | H;
	int level = v ? v - 1 : 0;
	if (d->log_packets) {
		decoder_say(d->dec, t, "packet F%d%d ABCD=%d EFGH=%d%d%d%d", b[8], b[9], v, E, F, G, H);
	}
	if (b[8] == b[9])
		set_function(d, t, 0, b[8]);

	if (d->mm1 || (E == A && F == B && G == C && H == D)) {
		/* the old format: speed code 1 reverses */
		if (v == 1) {
			if (!d->last_was_reverse) {
				bool toggle = d->mm1 || !d->seen_mm2;
				set_speed(d, t, 0, toggle ? !d->reverse : -1);
				if (!toggle)
					decoder_say(d->dec, t, "reverse packet: stop, the direction stays (MM2)");
			}
			d->last_was_reverse = true;
		} else {
			set_speed(d, t, level, -1);
			d->last_was_reverse = false;
		}
		return;
	}
	d->seen_mm2 = true;
	d->last_was_reverse = false;
	if ((efgh == 0xA || efgh == 0xB || efgh == 0x5 || efgh == 0x4) && H == !D) {
		set_speed(d, t, level, efgh >= 0xA);
		return;
	}
	int fn = 0;
	bool on = H;
	if (efgh == 0xA && !D) {           /* f1-f4 off where the standard code reads as MM1 */
		fn = v == 3 ? 1 : v == 4 ? 2 : v == 6 ? 3 : v == 7 ? 4 : 0;
		on = false;
	} else if (efgh == 0x5 && D) {     /* f1-f4 on, likewise */
		fn = v == 11 ? 1 : v == 12 ? 2 : v == 14 ? 3 : v == 15 ? 4 : 0;
		on = true;
	} else {
		int efg = efgh >> 1;
		fn = efg == 6 ? 1 : efg == 1 ? 2 : efg == 3 ? 3 : efg == 7 ? 4 : 0;
	}
	if (!fn) {
		d->unknown++;
		return;
	}
	set_function(d, t, fn, on);
	if (v != 1)
		set_speed(d, t, level, -1);
}

/* A complete packet: classify its bits, pair it with its repeat. */
static void packet(mm_t *d)
{
	int n = d->nbits;
	bool overflow = d->overflow;
	d->nbits = 0;
	d->overflow = false;
	if (n == 0)
		return;
	if (overflow || n != MM_BITS) {
		d->other++;
		return;
	}
	double cell = (d->rise[MM_BITS - 1] - d->rise[0]) * 1e6 / (MM_BITS - 1);
	bool solenoid = cell < (MM_LOCO_CELL_US + MM_SOLENOID_CELL_US) / 2;
	double nominal = solenoid ? MM_SOLENOID_CELL_US : MM_LOCO_CELL_US;
	uint8_t b[MM_BITS];
	for (int i = 0; i < MM_BITS; i++) {
		double frac = d->high_us[i] / nominal;
		if (i < MM_BITS - 1) {
			double c = (d->rise[i + 1] - d->rise[i]) * 1e6;
			if (c < nominal * 0.8 || c > nominal * 1.2) {
				d->bad_bits++;
				return;
			}
		}
		if (frac > 0.6)
			b[i] = 1;
		else if (frac < 0.4)
			b[i] = 0;
		else {
			d->other++;             /* symmetric pulses: not MM (mfx) */
			return;
		}
	}
	double start = d->rise[0], end = d->rise[MM_BITS - 1] + nominal * 1e-6;
	d->packets++;
	if (solenoid) {
		d->solenoid++;
	} else {
		d->loco++;
		for (int i = 0; i < MM_BITS; i++) {
			if (i < MM_BITS - 1)
				stat_add(&d->cell, (d->rise[i + 1] - d->rise[i]) * 1e6);
			stat_add(b[i] ? &d->high1 : &d->high0, d->high_us[i]);
		}
	}
	double gap = (start - d->prev_end) * 1e6;
	if (d->have_prev && d->prev_solenoid == solenoid && gap <= MM_REPEAT_US && !memcmp(b, d->prev, MM_BITS)) {
		d->have_prev = false;
		d->pairs++;
		if (!solenoid) {
			stat_add(&d->t1, gap);
			if (gap > MC145027_DEAD_TIME_US)
				d->t1_over++;
			d->pair_end = end;
			command(d, b, end);
		}
		return;
	}
	if (d->have_prev)
		d->unpaired++;
	if (!solenoid && d->pair_end >= 0) {
		double t2 = (start - d->pair_end) * 1e6;
		if (t2 < MM_SEQUENCE_US)
			stat_add(&d->t2, t2);
		d->pair_end = -1;
	}
	d->have_prev = true;
	memcpy(d->prev, b, MM_BITS);
	d->prev_solenoid = solenoid;
	d->prev_end = end;
}

static void mm_track(decoder_t *dec, double t, int pol)
{
	mm_t *d = dec->impl;
	if (pol == d->pol)
		return;
	double us = (t - d->t_last) * 1e6;   /* how long the level that ends now lasted */
	if (pol > 0) {
		if (d->pol < 0 && d->nbits && us > MM_PAUSE_US)
			packet(d);
		else if (d->pol == 0)
			d->nbits = 0, d->overflow = false, d->have_prev = false;
		if (d->nbits < MM_BITS)
			d->rise[d->nbits] = t;
		else
			d->overflow = true;
		d->in_pulse = true;
	} else if (pol < 0) {
		if (d->pol > 0 && d->in_pulse) {
			if (us > MM_PULSE_MAX_US)
				d->overflow = true;
			else if (d->nbits < MM_BITS)
				d->high_us[d->nbits++] = us;
		}
		d->in_pulse = false;
	} else {
		if (d->nbits)
			packet(d);
		d->in_pulse = false;
		d->have_prev = false;
	}
	if (d->pol == 0 && us > POWER_OFF_US) {
		stat_add(&d->off, us / 1e3);
		decoder_say(d->dec, t, "track voltage on after %.1f ms off", us / 1e3);
	}
	d->pol = pol;
	d->t_last = t;
}

static void mm_status(const decoder_t *dec, char *buf, size_t size)
{
	const mm_t *d = dec->impl;
	snprintf(buf, size, "%s  speed %s%d/14  F0 %s  f1-f4 %d%d%d%d  (%lu commands)%s", dec->name,
		d->reverse ? "-" : "+", d->speed, d->f[0] ? "on" : "off", d->f[1], d->f[2], d->f[3], d->f[4],
		d->commands, d->pol ? "" : "  no track voltage");
}

static void mm_report(decoder_t *dec, double t, FILE *out)
{
	mm_t *d = dec->impl;
	if (d->pol < 0 && d->nbits && (t - d->t_last) * 1e6 > MM_PAUSE_US)
		packet(d);
	char line[160];
	mm_status(dec, line, sizeof line);
	fprintf(out, "%10.6f %s\n", t, line);
	fprintf(out, "%s: MM packets %lu (locomotive %lu, solenoid %lu), double packets %lu, unpaired %lu\n",
		dec->name, d->packets, d->loco, d->solenoid, d->pairs, d->unpaired);
	fprintf(out, "  not MM (other signal) %lu, bad bit timing %lu, idle %lu, unknown codes %lu\n",
		d->other, d->bad_bits, d->idle, d->unknown);
	stat_print(out, "cell", &d->cell, "us");
	stat_print(out, "pulse of a 1", &d->high1, "us");
	stat_print(out, "pulse of a 0", &d->high0, "us");
	stat_print(out, "repeat pause t1", &d->t1, "us");
	if (d->t1_over)
		fprintf(out, "  %lu repeat pauses over the MC145027 limit (%.0f us)\n", d->t1_over, MC145027_DEAD_TIME_US);
	stat_print(out, "pause after a pair", &d->t2, "us");
	stat_print(out, "command interval", &d->refresh, "ms");
	stat_print(out, "track voltage off", &d->off, "ms");
	fprintf(out, "  commands by address:");
	for (int a = 1; a <= 80; a++)
		if (d->per_address[a])
			fprintf(out, " %d:%lu", a, d->per_address[a]);
	fputc('\n', out);
}

static const decoder_ops_t mm_ops = { mm_track, mm_status, mm_report, mm_free };
