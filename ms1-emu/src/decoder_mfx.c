/* The mfx locomotive decoder (mfx:UID).
 *
 * From S. Krauss, "Description of the mfx rail format" v2.3 (../doc/MFX - Trackprotocol.pdf):
 * - Bits: biphase mark at 10 kBaud. A 100 us cell starts with a polarity change; a 1 has a
 *   second change in its middle. The sync pattern 0 1 1 1 0 leaves out the changes between its
 *   2nd/3rd and 3rd/4th bits: a half cell followed by a full one breaks the coding rule and
 *   marks it. Each full pattern breaks it twice; a series can end in a half pattern (0 1 1),
 *   after which the data continues from that break.
 * - Frames, between sync patterns: address, commands, CRC (x^8+x^2+x+1, start 0x7F, over all
 *   bits including the CRC: 0), after removing the 0 stuffed after every eight 1s. Address:
 *   10+7, 110+9, 1110+11 or 1111+14 bits; 0 is the broadcast address.
 * - Commands: 000 R SSS drive (speed S*16), 001 R SSSSSSS drive (speed 1: emergency stop),
 *   010 FFFF, 011 0 F*8, 011 1 F*16 function states, 100 NNNNNNN 0 F one function, and
 *   111 CCC configuration: 000 V(10) I(6) CC read CV (1/2/4/8 bytes back), 001 V I 00 D(8)
 *   write CV, 010 C(6) U(32) search (1-bit answer from unregistered decoders whose UID matches
 *   U in its top C bits), 011 A(14) U(32) assign SID A to UID U, 100 U(32) ping (1-bit answer
 *   if U is the UID), 101 U(32) Z(16) the central's UID and registration counter.
 *   A frame without command asks for a 1-bit answer.
 * - Feedback window, after a command that wants an answer: 11 or 11.5 sync patterns, then
 *   0011, then a pause. A 1-bit answer is the carrier during the first 6.4 ms pause. For a
 *   data answer the central marks each bit clock (912 us) in the pause with a 25 us pulse,
 *   from the 24th clock on also the middle of the bit; the decoder sends 1s on the first 23
 *   clocks, then 010, the bytes (MSB first) and a checksum, then 1s.
 * - The checksum is the MS1's (mfx_feedback_data): a CRC with start 0xFF and polynomial
 *   0x07 over the data bits, as the document's algorithm in 2.3.4 also gives.
 * - Registration (4.2, 4.3): an unregistered decoder answers searches until the central has
 *   found its UID and assigned it a SID; it keeps the SID with the central's UID and
 *   registration counter, and registers again when a central command or a ping does not match
 *   them. After power-up a registered decoder takes commands only after two matching central
 *   commands.
 * The configuration variables (chapter 6) are a fixed image of a typical locomotive decoder:
 * the blocks and CA types the MS1's registration reads (mfx_registration_process). */
#include "decoder_int.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HALF_MIN_US 35.0
#define HALF_MAX_US 70.0
#define FULL_MIN_US 75.0
#define FULL_MAX_US 125.0
#define PULSE_MIN_US 10.0         /* shorter levels in a window: the polarity correction at its start */
#define PULSE_MAX_US 40.0         /* the central's clock marks in a data window */
#define CLOCK_US 912.0            /* data feedback bit clock (52.63 kHz / 48) */
#define IDLE_CLOCKS 23            /* clocks of 1s before the answer */
#define POWER_OFF_US 1000.0       /* 0 V this long is the track switched off */
#define RESTART_US 50000.0        /* 0 V this long restarts the decoder */
#define WINDOW_SYNCS 10           /* rule violations (two per sync pattern) before a window */
#define MAX_BITS 256
#define MAX_ANSWER (IDLE_CLOCKS + 3 + 9 * 8 + 8)

enum { RX_IDLE, RX_DATA, RX_WINDOW };
enum { FB_NONE, FB_1BIT, FB_DATA };

typedef struct {
	decoder_t *dec;
	uint32_t uid;
	bool log_frames;
	bool no_reads;                /* never answers a CV read (option noread) */
	uint8_t cv[0x10000];          /* by V << 6 | I */
	/* edges and bits */
	int pol;
	double t_last;
	int rx;
	bool half;
	int nbits;
	uint8_t bits[MAX_BITS];
	int violations;               /* sync rule violations since the last frame */
	/* feedback */
	int pending;                  /* the answer the last frame asked of this decoder */
	int pending_bytes;
	uint16_t pending_cv;
	int window;                   /* the answer being given in the current window */
	double window_start;
	bool edge_pending;            /* an edge after a pause: a clock mark or the closing sync */
	double edge_t;
	int clocks;
	double last_clock, first_level_us;
	uint8_t answer[MAX_ANSWER];
	int answer_len;
	/* the decoder */
	bool registered;
	int sid;
	uint32_t central_uid;         /* stored with the SID */
	uint16_t central_counter;
	bool central_seen;            /* a central command since power-up */
	uint32_t last_central_uid;    /* the last central command */
	uint16_t last_central_counter;
	int central_matches;          /* matching central commands since power-up */
	int speed;                    /* 0-127 */
	bool reverse, estop;
	uint32_t f;                   /* F0-F31 */
	double power_off_since;
	/* analysis */
	unsigned long frames, crc_errors, stuff_errors, violations_total, windows1, windows_data;
	unsigned long n_central, n_search, n_assign, n_ping, n_read, n_write, n_drive, n_func, n_unknown;
	unsigned long own_frames, answers1, answers_data;
	stat_t half_us, full_us, central_ms, pause1_us, pulse_us, clock_us, clocks_per_window, off;
	double last_central_t, own_first, own_last, assigned_t;
	unsigned long searches_answered;
} mfx_t;

static const decoder_ops_t mfx_ops;

/* ---- the configuration variables ---- */

static void cv_set(mfx_t *m, int ca, int index, const uint8_t *v, int n)
{
	for (int i = 0; i < n; i++)
		m->cv[(ca << 6 | (index + i)) & 0xFFFF] = v[i];
}

#define CA(m, ca, ...) do { const uint8_t v_[] = { __VA_ARGS__ }; cv_set(m, ca, 0, v_, (int)sizeof v_); } while (0)

/* Blocks (start CA / 4 in the block table):
 *   0x00 block 01 decoder properties: manufacturer, symbol, name, block table, protocol
 *   0x02 block 02 functions, one group per command code: group 0 the driving function (drive
 *        commands 00x), group 1 the switching functions (function commands 01x) with their
 *        groups in block 04; the MS1 takes the command codes from these group numbers
 *   0x03 block 04 function assignment: 16 groups of (symbol, forward, reverse)
 *   0x10 block 05 driving characteristics: motor, motor type, acceleration, trim, speed curve
 *   0x12 block 08 sound: volume */
static void cv_image(mfx_t *m, const char *name, uint8_t symbol)
{
	CA(m, 0x000, 0x01, 0x01, 0x01, 0x00, 0x01, 0x06);           /* block 01, 1 group of 6 CAs */
	CA(m, 0x001, 0x10, 0x00, 0x00, 0x00, 0x83, 0x00, 0x00, 0x00, 0x0E);   /* Märklin, product 0E */
	CA(m, 0x002, 0x16, 0x01, symbol, 0x00, 0x00, 0x83, 0x00, 0x00, 0x00);
	m->cv[0x003 << 6] = 0x18;                                      /* name */
	cv_set(m, 0x003, 1, (const uint8_t *)name, (int)strlen(name));
	CA(m, 0x004, 0x17, 0x02, 0x03, 0x10, 0x12, 0x00);             /* block table */
	CA(m, 0x005, 0x14, 0x01, 0x00, 0x40, 0x00, 0x05);             /* mfx 1.0, 64 bits, 2-byte reads */

	CA(m, 0x008, 0x01, 0x02, 0x01, 0x00, 0x02, 0x01);             /* block 02, 2 groups of 1 CA */
	CA(m, 0x009, 0x10, 0x01);                                      /* driving function: block 05 + 1 */
	CA(m, 0x00A, 0x11, 0x01, 0x04, 0x07, 0x0A, 0x0D, 0x10, 0x13, 0x16, 0x19, 0x1C, 0x1F, 0x22,
		0x25, 0x28, 0x2B, 0x2E);                                   /* F0-F15: groups in block 04 */

	CA(m, 0x00C, 0x01, 0x04, 0x01, 0x00, 0x10, 0x03);             /* block 04, 16 groups of 3 CAs */
	static const uint8_t symbols[16][3] = {                        /* M|G S1 S2 */
		{ 0x03, 0x03, 0x00 }, { 0x05, 0x05, 0x00 }, { 0x07, 0x07, 0x00 }, { 0x0A, 0x0A, 0x00 },
		{ 0x0B, 0x0B, 0x00 }, { 0x87, 0x07, 0x00 }, { 0x06, 0x06, 0x00 }, { 0x04, 0x04, 0x00 },
	};
	for (int g = 0; g < 16; g++) {
		int ca = 0x00D + 3 * g;
		const uint8_t *s = symbols[g][0] ? symbols[g] : (const uint8_t[]){ 0x02, 0x02, 0x00 };
		CA(m, ca, 0x10, s[0], s[1], s[2]);
		CA(m, ca + 1, 0x12, (uint8_t)(g < 8 ? 1 << g : 0), 0x00);
		CA(m, ca + 2, 0x13, (uint8_t)(g < 8 ? 1 << g : 0), 0x00);
	}

	CA(m, 0x040, 0x01, 0x05, 0x01, 0x00, 0x01, 0x07);             /* block 05, 1 group of 7 CAs */
	CA(m, 0x041, 0x10, 0x01);                                      /* one motor */
	CA(m, 0x042, 0x11, 0x03);                                      /* regulated DC motor */
	CA(m, 0x043, 0x13, 0x10, 0x0C);                                /* acceleration 4 s, braking 3 s */
	CA(m, 0x044, 0x14, 0x80, 0x80);                                /* trim */
	CA(m, 0x045, 0x17, 0x04, 0xC0);                                /* Vmin, Vmax */
	CA(m, 0x046, 0x18, 0x78, 0x00);                                /* speedometer 120 km/h */

	CA(m, 0x048, 0x01, 0x08, 0x01, 0x00, 0x01, 0x02);             /* block 08, 1 group of 2 CAs */
	CA(m, 0x049, 0x10, 0xC0);                                      /* volume */
}

/* ---- options ---- */

bool mfx_create(decoder_t *d, const char *args, char *err, size_t errsize)
{
	char *end;
	unsigned long uid = strtoul(args, &end, 16);
	if (end == args || end - args > 8) {
		snprintf(err, errsize, "decoder 'mfx:%s': the UID is up to 8 hex digits", args);
		return false;
	}
	mfx_t *m = calloc(1, sizeof *m);
	m->uid = (uint32_t)uid;
	char name[17] = "mfx lok";
	uint8_t symbol = 0x09;
	struct { unsigned cv, index, value; } set[16];
	int nset = 0;
	while (*end == ',') {
		const char *opt = end + 1;
		size_t n = strcspn(opt, ",");
		int used = 0;
		if (n == 6 && !strncmp(opt, "frames", 6)) {
			m->log_frames = true;
		} else if (n == 6 && !strncmp(opt, "noread", 6)) {
			m->no_reads = true;
		} else if (n > 3 && !strncmp(opt, "cv=", 3)) {
			if (nset == 16 || sscanf(opt + 3, "%x/%x:%x%n", &set[nset].cv, &set[nset].index, &set[nset].value, &used) != 3 ||
			    (size_t)used != n - 3 || set[nset].cv > 0x3FF || set[nset].index > 0x3F || set[nset].value > 0xFF) {
				snprintf(err, errsize, "decoder 'mfx:%s': cv=VVV/II:XX (hex), up to 16 of them", args);
				free(m);
				return false;
			}
			nset++;
		} else if (n > 5 && !strncmp(opt, "name=", 5) && n - 5 <= 16) {
			memcpy(name, opt + 5, n - 5);
			name[n - 5] = 0;
		} else if (n > 7 && !strncmp(opt, "symbol=", 7)) {
			static const char *const names[] = { "none", "elok", "diesel", "steam" };
			int k = 0;
			while (k < 4 && (strlen(names[k]) != n - 7 || strncmp(opt + 7, names[k], n - 7)))
				k++;
			if (k == 4) {
				snprintf(err, errsize, "decoder 'mfx:%s': symbol is none, elok, diesel or steam", args);
				free(m);
				return false;
			}
			symbol = (uint8_t)(0x08 + k);
		} else {
			snprintf(err, errsize, "decoder 'mfx:%s': unknown option '%.*s' (name is up to 16 characters)",
				args, (int)n, opt);
			free(m);
			return false;
		}
		end = (char *)opt + n;
	}
	if (*end) {
		snprintf(err, errsize, "decoder 'mfx:%s': unexpected '%s'", args, end);
		free(m);
		return false;
	}
	cv_image(m, name, symbol);
	for (int i = 0; i < nset; i++)
		m->cv[set[i].cv << 6 | set[i].index] = (uint8_t)set[i].value;
	m->dec = d;
	m->power_off_since = 0;
	m->last_central_t = m->own_first = m->assigned_t = -1;
	snprintf(d->name, sizeof d->name, "mfx:%08X", m->uid);
	d->ops = &mfx_ops;
	d->impl = m;
	return true;
}

static void mfx_free(decoder_t *d)
{
	free(d->impl);
}

/* ---- the decoder ---- */

static void say(mfx_t *m, double t, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void say(mfx_t *m, double t, const char *fmt, ...)
{
	char text[200];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof text, fmt, ap);
	va_end(ap);
	decoder_say(m->dec, t, "%s", text);
}

static bool accepts(const mfx_t *m)
{
	return m->registered && m->central_matches >= 2;
}

static void unregister(mfx_t *m, double t, const char *why)
{
	if (m->registered)
		say(m, t, "%s: registering again", why);
	m->registered = false;
}

static void drive(mfx_t *m, double t, int reverse, int speed)
{
	if (speed == 1) {
		if (!m->estop)
			say(m, t, "emergency stop");
		m->estop = true;
		m->speed = 0;
		m->reverse = reverse;
		return;
	}
	m->estop = false;
	if (speed != m->speed || reverse != m->reverse) {
		m->speed = speed;
		m->reverse = reverse;
		say(m, t, "speed %c%d", reverse ? '-' : '+', speed);
	}
}

static void functions(mfx_t *m, double t, uint32_t mask, uint32_t value)
{
	uint32_t changed = (m->f ^ value) & mask;
	m->f = (m->f & ~mask) | (value & mask);
	for (int i = 0; i < 32; i++)
		if (changed >> i & 1)
			say(m, t, "F%d %s", i, value >> i & 1 ? "on" : "off");
}

static uint8_t checksum(const uint8_t *data, int n)
{
	uint8_t c = 0xFF;
	for (int i = 0; i < n; i++)
		for (int k = 7; k >= 0; k--) {
			int b = data[i] >> k & 1;
			c = (uint8_t)(c & 0x80 ? (c << 1) ^ b ^ 0x07 : (c << 1) ^ b);
		}
	return c;
}

/* The bit stream of a data answer: 1s, 010, the bytes, the checksum, 1s. */
static void prepare_answer(mfx_t *m, double t)
{
	uint8_t data[8];
	int n = m->pending_bytes, k = 0;
	for (int i = 0; i < n; i++)
		data[i] = m->cv[(m->pending_cv & 0xFFC0) | ((m->pending_cv + i) & 0x3F)];
	data[n] = checksum(data, n);
	for (int i = 0; i < IDLE_CLOCKS; i++)
		m->answer[k++] = 1;
	m->answer[k++] = 0;
	m->answer[k++] = 1;
	m->answer[k++] = 0;
	for (int i = 0; i <= n; i++)
		for (int b = 7; b >= 0; b--)
			m->answer[k++] = data[i] >> b & 1;
	m->answer_len = k;
	char hex[40];
	int h = 0;
	for (int i = 0; i < n; i++)
		h += snprintf(hex + h, sizeof hex - (size_t)h, " %02X", data[i]);
	say(m, t, "read CV %03X/%02X:%s", m->pending_cv >> 6, m->pending_cv & 0x3F, hex);
}

/* ---- frames ---- */

static uint32_t take(const uint8_t *b, int *i, int n)
{
	uint32_t v = 0;
	while (n--)
		v = v << 1 | b[(*i)++];
	return v;
}

static void frame(mfx_t *m, double t, const uint8_t *raw, int nraw)
{
	uint8_t b[MAX_BITS];
	int n = 0, ones = 0;
	for (int i = 0; i < nraw; i++) {
		if (ones == 8) {               /* the stuffed 0 */
			ones = 0;
			if (raw[i]) {
				m->stuff_errors++;
				return;
			}
			continue;
		}
		b[n++] = raw[i];
		ones = raw[i] ? ones + 1 : 0;
	}
	unsigned crc = 0x7F;
	for (int i = 0; i < n; i++) {
		crc = crc << 1 | b[i];
		if (crc & 0x100)
			crc ^= 0x107;
	}
	if (n < 9 + 8 || crc) {
		m->crc_errors++;
		if (m->log_frames)
			say(m, t, "frame of %d bits: CRC error", n);
		return;
	}
	m->frames++;
	m->pending = FB_NONE;
	int i, end = n - 8, sid;
	if (!b[0])
		return;
	if (!b[1])
		i = 2, sid = (int)take(b, &i, 7);
	else if (!b[2])
		i = 3, sid = (int)take(b, &i, 9);
	else if (!b[3])
		i = 4, sid = (int)take(b, &i, 11);
	else
		i = 4, sid = (int)take(b, &i, 14);
	bool own = sid != 0 && m->registered && sid == m->sid;
	if (own) {
		m->own_frames++;
		if (m->own_first < 0)
			m->own_first = t;
		m->own_last = t;
	}
	char text[160];
	int tl = snprintf(text, sizeof text, "frame SID %d:", sid);
	if (i == end && own && accepts(m))
		m->pending = FB_1BIT;
	while (i + 3 <= end) {
		int cmd = (int)take(b, &i, 3);
		bool to_us = (own && accepts(m)) || (sid == 0 && m->registered);
		if (cmd <= 1) {
			int r = (int)take(b, &i, 1), s = (int)take(b, &i, cmd ? 7 : 3);
			int speed = cmd ? s : s * 16;
			m->n_drive++;
			tl += snprintf(text + tl, sizeof text - (size_t)tl, " drive %c%d", r ? '-' : '+', speed);
			if (to_us)
				drive(m, t, r, speed);
		} else if (cmd == 2 || cmd == 3) {
			int w = cmd == 2 ? 4 : take(b, &i, 1) ? 16 : 8;
			uint32_t v = take(b, &i, w);
			m->n_func++;
			tl += snprintf(text + tl, sizeof text - (size_t)tl, " F0-F%d %0*X", w - 1, w / 4, v);
			if (to_us)
				functions(m, t, 0xFFFF, v);
		} else if (cmd == 4) {
			int fn = (int)take(b, &i, 7);
			take(b, &i, 1);
			int on = (int)take(b, &i, 1);
			m->n_func++;
			tl += snprintf(text + tl, sizeof text - (size_t)tl, " F%d=%d", fn, on);
			if (to_us && fn < 32)
				functions(m, t, 1u << fn, (uint32_t)on << fn);
		} else if (cmd == 7 && i + 3 <= end) {
			int sub = (int)take(b, &i, 3);
			if (sub == 0 && i + 18 <= end) {
				uint16_t cv = (uint16_t)take(b, &i, 16);
				int bytes = 1 << take(b, &i, 2);
				m->n_read++;
				tl += snprintf(text + tl, sizeof text - (size_t)tl, " read CV %03X/%02X x%d", cv >> 6, cv & 0x3F, bytes);
				if (own && accepts(m) && !m->no_reads) {
					m->pending = FB_DATA;
					m->pending_cv = cv;
					m->pending_bytes = bytes;
				}
			} else if (sub == 1 && i + 26 <= end) {
				uint16_t cv = (uint16_t)take(b, &i, 16);
				take(b, &i, 2);
				uint8_t v = (uint8_t)take(b, &i, 8);
				m->n_write++;
				tl += snprintf(text + tl, sizeof text - (size_t)tl, " write CV %03X/%02X=%02X", cv >> 6, cv & 0x3F, v);
				if (own && accepts(m) && (cv & 0x3F) != 0 && m->cv[cv] != v) {
					m->cv[cv] = v;
					say(m, t, "write CV %03X/%02X = %02X", cv >> 6, cv & 0x3F, v);
				}
			} else if (sub == 2 && i + 38 <= end) {
				int c = (int)take(b, &i, 6);
				uint32_t u = take(b, &i, 32);
				m->n_search++;
				tl += snprintf(text + tl, sizeof text - (size_t)tl, " search C%d U%08X", c, u);
				uint32_t mask = c ? 0xFFFFFFFFu << (32 - (c > 32 ? 32 : c)) : 0;
				if (sid == 0 && !m->registered && ((u ^ m->uid) & mask) == 0) {
					m->pending = FB_1BIT;
					m->searches_answered++;
					if (c == 0 || c >= 32)
						say(m, t, "search C%d: answering", c);
				}
			} else if (sub == 3 && i + 46 <= end) {
				int a = (int)take(b, &i, 14);
				uint32_t u = take(b, &i, 32);
				m->n_assign++;
				tl += snprintf(text + tl, sizeof text - (size_t)tl, " assign SID %d to %08X", a, u);
				if (u == m->uid && (!m->registered || m->sid != a)) {
					m->registered = true;
					m->sid = a;
					m->central_uid = m->last_central_uid;
					m->central_counter = m->last_central_counter;
					m->central_matches = 2;
					m->assigned_t = t;
					say(m, t, "registered: SID %d (central %08X, counter %u)", a, m->central_uid, m->central_counter);
				}
			} else if (sub == 4 && i + 32 <= end) {
				uint32_t u = take(b, &i, 32);
				m->n_ping++;
				tl += snprintf(text + tl, sizeof text - (size_t)tl, " ping %08X", u);
				if (own && accepts(m)) {
					if (u == m->uid)
						m->pending = FB_1BIT;
					else
						unregister(m, t, "ping with another UID");
				}
			} else if (sub == 5 && i + 48 <= end) {
				uint32_t u = take(b, &i, 32);
				uint16_t z = (uint16_t)take(b, &i, 16);
				m->n_central++;
				tl += snprintf(text + tl, sizeof text - (size_t)tl, " central %08X counter %u", u, z);
				if (m->last_central_t >= 0)
					stat_add(&m->central_ms, (t - m->last_central_t) * 1e3);
				m->last_central_t = t;
				if (!m->central_seen || u != m->last_central_uid || z != m->last_central_counter)
					say(m, t, "central %08X, registration counter %u", u, z);
				if (m->registered) {
					if (u == m->central_uid && z == m->central_counter) {
						if (m->central_matches < 2 && ++m->central_matches == 2)
							say(m, t, "the central is known: taking commands");
					} else if (u == m->central_uid && m->central_seen && m->last_central_uid == u &&
						   m->last_central_counter == m->central_counter) {
						m->central_counter = z;   /* counted on while this decoder listened */
					} else {
						unregister(m, t, "another central or registration counter");
					}
				}
				m->central_seen = true;
				m->last_central_uid = u;
				m->last_central_counter = z;
			} else {
				m->n_unknown++;
				tl += snprintf(text + tl, sizeof text - (size_t)tl, " 111 %d?", sub);
				break;
			}
		} else {
			m->n_unknown++;
			tl += snprintf(text + tl, sizeof text - (size_t)tl, " command %d?", cmd);
			break;
		}
		if (tl >= (int)sizeof text)
			tl = (int)sizeof text - 1;
	}
	if (m->log_frames)
		say(m, t, "%s", text);
}

/* ---- feedback windows ---- */

static void window_start(mfx_t *m, double t)
{
	m->rx = RX_WINDOW;
	m->window = m->pending;
	m->pending = FB_NONE;
	m->window_start = t;
	m->edge_pending = false;
	m->clocks = 0;
	m->first_level_us = 0;
	if (m->window == FB_DATA)
		prepare_answer(m, t);
	if (m->window != FB_NONE && m->dec->fb.carrier)
		m->dec->fb.carrier(m->dec->fb.ctx, true);
}

static void window_end(mfx_t *m, double t)
{
	if (m->clocks) {
		m->windows_data++;
		stat_add(&m->clocks_per_window, m->clocks);
	} else {
		m->windows1++;
		stat_add(&m->pause1_us, m->first_level_us);
	}
	if (m->window == FB_DATA) {
		m->answers_data++;
		if (m->dec->fb.carrier)
			m->dec->fb.carrier(m->dec->fb.ctx, false);
		if (m->clocks < m->answer_len)
			say(m, t, "the window ended after %d clocks, the answer needs %d", m->clocks, m->answer_len);
	}
	m->window = FB_NONE;
	m->rx = RX_IDLE;
	m->half = false;
	m->nbits = 0;
}

/* A 25 us mark of the central at tp. */
static void pulse(mfx_t *m, double tp)
{
	if (m->clocks && (tp - m->last_clock) * 1e6 < CLOCK_US * 0.75)
		return;                       /* a mid-bit mark */
	if (m->clocks)
		stat_add(&m->clock_us, (tp - m->last_clock) * 1e6);
	if (m->window == FB_DATA && m->dec->fb.bit)
		m->dec->fb.bit(m->dec->fb.ctx, m->clocks < m->answer_len ? m->answer[m->clocks] : 1);
	m->clocks++;
	m->last_clock = tp;
}

/* ---- edges ---- */

static void bit(mfx_t *m, double t, int v)
{
	if (m->nbits < MAX_BITS)
		m->bits[m->nbits++] = (uint8_t)v;
	/* 0011 right after sync patterns: a feedback window starts with this edge */
	if (m->nbits == 4 && m->violations >= WINDOW_SYNCS && !m->bits[0] && !m->bits[1] && m->bits[2] && m->bits[3])
		window_start(m, t);
}

/* A level of us microseconds ended at t: biphase mark decoding. */
static void cell(mfx_t *m, double t, double us)
{
	bool h = us >= HALF_MIN_US && us <= HALF_MAX_US, f = us >= FULL_MIN_US && us <= FULL_MAX_US;
	if (!h && !f) {
		m->rx = RX_IDLE;
		m->half = false;
		m->nbits = 0;
		m->violations = 0;
		return;
	}
	stat_add(h ? &m->half_us : &m->full_us, us);
	if (h) {
		if (m->half)
			bit(m, t, 1);
		m->half = !m->half;
		return;
	}
	if (!m->half) {
		bit(m, t, 0);
		return;
	}
	/* a half cell, then a full one: a sync pattern; the 0 before it was its first bit */
	if (m->rx == RX_DATA && m->nbits > 1)
		frame(m, t, m->bits, m->nbits - 1);
	if (m->rx == RX_DATA && m->nbits > 1)
		m->violations = 0;
	m->violations++;
	m->violations_total++;
	m->rx = RX_DATA;
	m->half = false;
	m->nbits = 0;
}

static void in_window(mfx_t *m, double t, double us)
{
	if (us < PULSE_MIN_US)
		return;
	if (m->edge_pending) {
		m->edge_pending = false;
		if (us <= PULSE_MAX_US) {
			stat_add(&m->pulse_us, us);
			pulse(m, m->edge_t);
			return;
		}
		/* the closing sync pattern started at the pending edge */
		window_end(m, m->edge_t);
		cell(m, t, us);
		return;
	}
	if (m->first_level_us == 0)
		m->first_level_us = us;
	if (m->window == FB_1BIT) {
		/* the first pause is over */
		m->answers1++;
		m->window = FB_NONE;
		if (m->dec->fb.carrier)
			m->dec->fb.carrier(m->dec->fb.ctx, false);
	}
	m->edge_pending = true;
	m->edge_t = t;
}

static void mfx_track(decoder_t *d, double t, int pol)
{
	mfx_t *m = d->impl;
	if (pol == m->pol)
		return;
	double us = (t - m->t_last) * 1e6;
	if (m->pol == 0) {
		if (us > POWER_OFF_US) {
			stat_add(&m->off, us / 1e3);
			say(m, t, "track voltage on after %.1f ms off", us / 1e3);
		}
		if (us > RESTART_US) {
			m->central_seen = false;
			m->central_matches = 0;
		}
	}
	if (pol == 0) {
		if (m->rx == RX_WINDOW)
			window_end(m, t);
		m->rx = RX_IDLE;
		m->half = false;
		m->nbits = 0;
		m->violations = 0;
	} else if (m->pol != 0) {
		if (m->rx == RX_WINDOW)
			in_window(m, t, us);
		else
			cell(m, t, us);
	}
	m->pol = pol;
	if (m->rx != RX_WINDOW || us >= PULSE_MIN_US || pol == 0)
		m->t_last = t;
}

static void mfx_status(const decoder_t *d, char *buf, size_t size)
{
	const mfx_t *m = d->impl;
	char reg[48];
	if (m->registered)
		snprintf(reg, sizeof reg, "SID %d%s", m->sid, accepts(m) ? "" : " (waiting for the central)");
	else
		snprintf(reg, sizeof reg, "not registered");
	snprintf(buf, size, "%s  %s  speed %c%d/127  F0-F15 %04X  (%lu frames)%s", d->name, reg,
		m->reverse ? '-' : '+', m->speed, m->f & 0xFFFF, m->own_frames, m->pol ? "" : "  no track voltage");
}

static void mfx_report(decoder_t *d, double t, FILE *out)
{
	mfx_t *m = d->impl;
	char line[200];
	mfx_status(d, line, sizeof line);
	fprintf(out, "%10.6f %s\n", t, line);
	fprintf(out, "%s: mfx frames %lu, CRC errors %lu, stuffing errors %lu, sync patterns %lu\n", d->name,
		m->frames, m->crc_errors, m->stuff_errors, m->violations_total / 2);
	fprintf(out, "  commands: central %lu, search %lu, assign %lu, ping %lu, read CV %lu, write CV %lu, "
		"drive %lu, functions %lu, unknown %lu\n", m->n_central, m->n_search, m->n_assign, m->n_ping,
		m->n_read, m->n_write, m->n_drive, m->n_func, m->n_unknown);
	stat_print(out, "half cell", &m->half_us, "us");
	stat_print(out, "full cell", &m->full_us, "us");
	stat_print(out, "central interval", &m->central_ms, "ms");
	fprintf(out, "  feedback windows: 1-bit %lu, data %lu; answered: 1-bit %lu (searches %lu), data %lu\n",
		m->windows1, m->windows_data, m->answers1, m->searches_answered, m->answers_data);
	stat_print(out, "1-bit window pause", &m->pause1_us, "us");
	stat_print(out, "clock marks per window", &m->clocks_per_window, "");
	stat_print(out, "clock interval", &m->clock_us, "us");
	stat_print(out, "clock mark width", &m->pulse_us, "us");
	stat_print(out, "track voltage off", &m->off, "ms");
	if (m->assigned_t >= 0)
		fprintf(out, "  registered at %.3f s; frames for SID %d: %lu (%.3f-%.3f s)\n", m->assigned_t, m->sid,
			m->own_frames, m->own_first, m->own_last);
}

static const decoder_ops_t mfx_ops = { mfx_track, mfx_status, mfx_report, mfx_free };
