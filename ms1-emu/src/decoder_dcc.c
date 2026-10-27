/* The DCC decoders: locomotive (dcc:ADDR) and basic accessory (dccacc:ADDR).
 *
 * DCC as the RailCommunity standards define it (RCN-210 bits, RCN-211 packets, RCN-212
 * locomotive commands, RCN-213 accessory commands, RCN-214 CV access, RCN-216 service mode,
 * RCN-225 CVs; NMRA S-9.2, S-9.2.1, S-9.2.3), with the receiver of the ZIMO DCC library
 * (github.com/ZIMO-Elektronik/DCC, rx) as the reference for what a decoder accepts:
 * - Each level between two edges is half a bit: 52-64 us for a 1, 90-10000 us for a 0 (a
 *   stretched 0); anything else is no DCC and restarts the receiver. A bit is two halves of
 *   the same kind.
 * - A packet: a preamble of at least 10 ones, then each byte after a 0 start bit, a 1 end bit
 *   after the last; the XOR of all bytes is 0; at least 3 bytes. The end bit counts as the
 *   first preamble bit of the next packet.
 * - The first byte is the address: 0 broadcast, 1-127 a locomotive (short), 128-191 an
 *   accessory (with bits of the second byte), 192-231 a locomotive (long, 14 bits with the
 *   second byte), 255 the idle packet.
 * - Locomotive commands (RCN-212): 0x3F speed in 128 steps (DSSSSSSS: 0 stop, 1 emergency
 *   stop, 2-127 steps 1-126, D forward); 01DCSSSS speed in 28 steps (C the low bit; codes 0-1
 *   stop, 2-3 emergency stop, 4-31 steps 1-28) or, with CV29 bit 1 clear, 14 steps (C is F0);
 *   100FFFFF F0 (bit 4) and F1-F4; 1011FFFF F5-F8; 1010FFFF F9-F12; 0xDE F13-F20; 0xDF
 *   F21-F28.
 * - CV access on the main (POM, RCN-214): 1110CCVV VVVVVVVV DDDDDDDD, CV = V + 1; CC 11 write
 *   the byte, 10 a bit (DDDDDDDD = 111KDBBB, K = 1 write), 01 verify (answered only by
 *   RailCom, which the MS1 lacks). A decoder acts on the second of two identical packets.
 * - Service mode (RCN-216, ZIMO's model): a broadcast reset packet (0x00 0x00) enters it; a
 *   packet that is neither a reset nor 0111xxxx leaves it. Direct mode 0111CCVV VVVVVVVV
 *   DDDDDDDD, same codes as POM, acted on at the second of two identical packets: a verify that
 *   matches, and every write, answer with an acknowledgement, a current pulse of ACK_MA for
 *   ACK_S through the board (decoder_feedback_t.current). Register and paged mode (3-byte
 *   0111xxxx packets) are not modelled.
 * - CVs (RCN-225): CV1 the short address (a write clears CV29 bit 5, as ZIMO's decoder does),
 *   CV17/CV18 the long address, CV29 bit 5 selects it, bit 1 28/128 speed steps; CV7 version,
 *   CV8 manufacturer, read-only; writing 8 to CV8 resets the CVs. An accessory decoder's
 *   address is CV1 (bits 0-5) and CV9 (bits 6-8).
 * - Basic accessory packets (RCN-213): 10AAAAAA 1aaaCPPO with aaa the complement of address
 *   bits 6-8: decoder address (9 bits), port PP (0-3), output O, C on/off. Output address
 *   (what a user types) = (decoder address - 1) * 4 + port + 1. Extended (signal) packets
 *   (1aaa0PP1 + aspect byte) are logged. */
#include "decoder_int.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BIT1_MIN_US 52.0
#define BIT1_MAX_US 64.0
#define BIT0_MIN_US 90.0
#define BIT0_MAX_US 10000.0
#define MIN_PREAMBLE 10           /* bits */
#define MAX_BYTES 8
#define ACK_MA 100.0              /* acknowledgement: current pulse, RCN-216: >= 60 mA */
#define ACK_S 6e-3                /* RCN-216: 6 ms +- 1 ms */
#define POWER_OFF_US 1000.0       /* 0 V this long is the track switched off (STOP) */
#define NCV 1024

typedef enum { HUNT, START, DATA, END } rx_state_t;

typedef struct {
	decoder_t *dec;
	bool accessory, noack, log_packets;
	int ack_ma;
	/* edges and the bit receiver */
	int pol;
	double t_last;
	rx_state_t state;
	int ones;                 /* 1 halves in a row: the preamble */
	int half;                 /* the first half of the bit being received: -1 none, 0, 1 */
	double half_us;
	int nbits, nbytes;
	uint8_t byte, bytes[MAX_BYTES];
	int preamble;             /* bits of the packet being received */
	stat_t ph1, ph0;          /* its halves, counted in h1 / h0 when it is a valid packet */
	/* the decoder */
	uint8_t cv[NCV + 1];
	bool service;
	uint8_t last[MAX_BYTES];  /* the last packet for this decoder, for the repeat rule */
	int last_n, repeats;
	int speed, steps;         /* steps 0-126 / 28 / 14, as last commanded */
	bool forward, estop;
	uint32_t f;               /* F0-F28 */
	uint8_t outputs[4];       /* accessory: per port, bit o set while output o is on */
	int aspect;
	double last_own;
	/* analysis */
	unsigned long packets, bad_xor, too_short, idle, resets, broadcast, own, other, accessory_packets;
	unsigned long acks, service_packets, unknown;
	stat_t h1, h0, pre, interval, off, asym1;
	unsigned long per_address[10240];
} dcc_t;

static const decoder_ops_t dcc_ops;

static int address(const dcc_t *d)
{
	if (d->accessory)
		return (d->cv[1] & 0x3F) | (d->cv[9] & 0x07) << 6;
	if (d->cv[29] & 0x20)
		return (d->cv[17] & 0x3F) << 8 | d->cv[18];
	return d->cv[1];
}

static void cv_defaults(dcc_t *d, int addr)
{
	memset(d->cv, 0, sizeof d->cv);
	d->cv[7] = 1;                    /* version */
	d->cv[8] = 13;                   /* NMRA manufacturer ID 13: public domain and DIY */
	if (d->accessory) {
		d->cv[1] = (uint8_t)(addr & 0x3F);
		d->cv[9] = (uint8_t)(addr >> 6 & 0x07);
		d->cv[29] = 0x80;
		return;
	}
	d->cv[29] = 0x02;                /* 28/128 speed steps */
	if (addr > 127) {
		d->cv[17] = (uint8_t)(0xC0 | addr >> 8);
		d->cv[18] = (uint8_t)addr;
		d->cv[29] |= 0x20;
		d->cv[1] = 3;
	} else {
		d->cv[1] = (uint8_t)addr;
	}
	d->cv[3] = d->cv[4] = 1;
}

static bool option(const char *opt, size_t n, const char *name)
{
	return n == strlen(name) && !strncmp(opt, name, n);
}

static bool create(decoder_t *dec, const char *args, bool accessory, char *err, size_t errsize)
{
	const char *kind = accessory ? "dccacc" : "dcc";
	char *end;
	long a = strtol(args, &end, 10);
	if (end == args || a < 1 || a > (accessory ? 510 : 10239)) {
		snprintf(err, errsize, "decoder '%s:%s': the address is 1-%d", kind, args, accessory ? 510 : 10239);
		return false;
	}
	dcc_t *d = calloc(1, sizeof *d);
	d->accessory = accessory;
	d->ack_ma = (int)ACK_MA;
	cv_defaults(d, (int)a);
	bool long_addr = false;
	while (*end == ',') {
		const char *opt = end + 1;
		size_t n = strcspn(opt, ",");
		unsigned cvn, val;
		int used;
		if (option(opt, n, "packets")) {
			d->log_packets = true;
		} else if (option(opt, n, "noack")) {
			d->noack = true;
		} else if (!accessory && option(opt, n, "long")) {
			long_addr = true;
		} else if (!accessory && sscanf(opt, "steps=%u%n", &val, &used) == 1 && (size_t)used == n
			   && (val == 14 || val == 28 || val == 128)) {
			d->cv[29] = (uint8_t)(val == 14 ? d->cv[29] & ~0x02 : d->cv[29] | 0x02);
		} else if (sscanf(opt, "ack=%u%n", &val, &used) == 1 && (size_t)used == n && val <= 1000) {
			d->ack_ma = (int)val;
		} else if (sscanf(opt, "cv=%u:%u%n", &cvn, &val, &used) == 2 && (size_t)used == n && cvn >= 1
			   && cvn <= NCV && val <= 255) {
			d->cv[cvn] = (uint8_t)val;
		} else {
			snprintf(err, errsize, "decoder '%s:%s': unknown option '%.*s'", kind, args, (int)n, opt);
			free(d);
			return false;
		}
		end = (char *)opt + n;
	}
	if (*end) {
		snprintf(err, errsize, "decoder '%s:%s': unexpected '%s'", kind, args, end);
		free(d);
		return false;
	}
	if (long_addr && a <= 127) {
		d->cv[17] = 0xC0;
		d->cv[18] = (uint8_t)a;
		d->cv[29] |= 0x20;
	}
	snprintf(dec->name, sizeof dec->name, "%s:%ld", kind, a);
	d->dec = dec;
	d->half = -1;
	d->last_own = -1;
	d->forward = true;
	d->steps = accessory ? 0 : 126;
	dec->ops = &dcc_ops;
	dec->impl = d;
	return true;
}

bool dcc_create(decoder_t *d, const char *args, char *err, size_t errsize)
{
	return create(d, args, false, err, errsize);
}

bool dccacc_create(decoder_t *d, const char *args, char *err, size_t errsize)
{
	return create(d, args, true, err, errsize);
}

static void dcc_free(decoder_t *d)
{
	free(d->impl);
}

/* ---- the decoder ---- */

static void ack(dcc_t *d, double t)
{
	d->acks++;
	if (d->noack)
		return;
	if (d->dec->fb.current)
		d->dec->fb.current(d->dec->fb.ctx, d->ack_ma, ACK_S);
	decoder_say(d->dec, t, "ack");
}

static void set_speed(dcc_t *d, double t, int speed, int steps, bool forward, bool estop)
{
	if (speed == d->speed && steps == d->steps && forward == d->forward && estop == d->estop)
		return;
	d->speed = speed;
	d->steps = steps;
	d->forward = forward;
	d->estop = estop;
	if (estop)
		decoder_say(d->dec, t, "emergency stop %s", forward ? "+" : "-");
	else
		decoder_say(d->dec, t, "speed %s%d/%d", forward ? "+" : "-", speed, steps);
}

static void set_functions(dcc_t *d, double t, int first, int count, unsigned bits)
{
	for (int i = 0; i < count; i++) {
		uint32_t mask = 1u << (first + i);
		bool on = bits >> i & 1;
		if (!(d->f & mask) != !on) {
			d->f ^= mask;
			decoder_say(d->dec, t, "F%d %s", first + i, on ? "on" : "off");
		}
	}
}

static void cv_write(dcc_t *d, double t, int cv, uint8_t value, const char *how)
{
	if (cv == 8 && value == 8) {
		cv_defaults(d, d->accessory ? 1 : 3);
		decoder_say(d->dec, t, "CV 8 = 8: CVs reset (%s)", how);
		return;
	}
	if (cv == 7 || cv == 8) {
		decoder_say(d->dec, t, "CV %d = %d: read-only (%s)", cv, value, how);
		return;
	}
	d->cv[cv] = value;
	if (cv == 1 && !d->accessory)
		d->cv[29] &= (uint8_t)~0x20;
	decoder_say(d->dec, t, "CV %d = %d (%s)", cv, value, how);
}

/* CV access long form, bytes from 1110CCVV / 0111CCVV on; true when it answers (service) */
static bool cv_access(dcc_t *d, double t, const uint8_t *b, bool service)
{
	int cc = b[0] >> 2 & 3, cv = ((b[0] & 3) << 8 | b[1]) + 1;
	uint8_t v = b[2];
	const char *how = service ? "service" : "POM";
	switch (cc) {
	case 1:
		if (!service) {
			decoder_say(d->dec, t, "verify CV %d = %d (POM: answered only by RailCom)", cv, v);
			return false;
		}
		decoder_say(d->dec, t, "verify CV %d = %d%s", cv, v, d->cv[cv] == v ? "" : ": no");
		return d->cv[cv] == v;
	case 3:
		cv_write(d, t, cv, v, how);
		return true;
	case 2: {
		int k = v >> 4 & 1, bit = v >> 3 & 1, pos = v & 7;
		if ((v & 0xE0) != 0xE0) {
			d->unknown++;
			return false;
		}
		if (k) {
			cv_write(d, t, cv, (uint8_t)((d->cv[cv] & ~(1 << pos)) | bit << pos), how);
			return true;
		}
		if (!service)
			return false;
		bool match = (d->cv[cv] >> pos & 1) == bit;
		decoder_say(d->dec, t, "verify CV %d bit %d = %d%s", cv, pos, bit, match ? "" : ": no");
		return match;
	}
	default:
		d->unknown++;
		return false;
	}
}

/* count a packet for this decoder: true at the second of two identical packets in a row */
static bool repeated(dcc_t *d, const uint8_t *b, int n)
{
	if (n == d->last_n && !memcmp(b, d->last, (size_t)n))
		d->repeats++;
	else
		d->repeats = 1;
	memcpy(d->last, b, (size_t)n);
	d->last_n = n;
	return d->repeats == 2;
}

static void service_packet(dcc_t *d, double t, const uint8_t *b, int n)
{
	d->service_packets++;
	if (n == 2 && !b[0] && !b[1])
		return;                          /* reset */
	if ((b[0] & 0xF0) != 0x70) {
		d->service = false;
		d->last_n = 0;
		decoder_say(d->dec, t, "service mode off");
		return;
	}
	if (n != 3) {                        /* without the XOR byte: direct mode has 3 */
		decoder_say(d->dec, t, "register/paged mode packet: not modelled");
		return;
	}
	if (repeated(d, b, n) && cv_access(d, t, b, true))
		ack(d, t);
}

/* An instruction for this locomotive decoder (or broadcast); again: the second of two
 * identical packets in a row for it */
static void loco_instruction(dcc_t *d, double t, const uint8_t *b, int n, bool bcast, bool again)
{
	uint8_t i = b[0];
	if (i == 0x3F && n >= 2) {
		int s = b[1] & 0x7F;
		bool fwd = b[1] >> 7;
		set_speed(d, t, s >= 2 ? s - 1 : 0, 126, fwd, s == 1);
	} else if ((i & 0xC0) == 0x40) {
		bool fwd = i >> 5 & 1;
		if (d->cv[29] & 0x02) {
			int s = (i & 0x0F) << 1 | (i >> 4 & 1);
			set_speed(d, t, s >= 4 ? s - 3 : 0, 28, fwd, s == 2 || s == 3);
		} else {
			int s = i & 0x0F;
			set_speed(d, t, s >= 2 ? s - 1 : 0, 14, fwd, s == 1);
			set_functions(d, t, 0, 1, i >> 4 & 1);
		}
	} else if ((i & 0xE0) == 0x80) {
		if (d->cv[29] & 0x02)
			set_functions(d, t, 0, 1, i >> 4 & 1);
		set_functions(d, t, 1, 4, i & 0x0F);
	} else if ((i & 0xF0) == 0xB0) {
		set_functions(d, t, 5, 4, i & 0x0F);
	} else if ((i & 0xF0) == 0xA0) {
		set_functions(d, t, 9, 4, i & 0x0F);
	} else if ((i == 0xDE || i == 0xDF) && n >= 2) {
		set_functions(d, t, i == 0xDE ? 13 : 21, 8, b[1]);
	} else if ((i & 0xF0) == 0xE0 && n >= 3 && !bcast) {
		if (again)
			cv_access(d, t, b, false);
	} else if (i == 0x00 || i == 0x01) {
		decoder_say(d->dec, t, "decoder %s reset", i ? "hard" : "digital");
	} else {
		d->unknown++;
	}
}

static void accessory_packet(dcc_t *d, double t, const uint8_t *b, int n)
{
	int addr = (b[0] & 0x3F) | (~b[1] >> 4 & 7) << 6;
	if (addr != address(d))
		return;
	d->own++;
	int port = b[1] >> 1 & 3;
	if (b[1] & 0x80) {                     /* basic */
		int o = b[1] & 1, on = b[1] >> 3 & 1;
		uint8_t before = d->outputs[port];
		d->outputs[port] = (uint8_t)(on ? before | 1 << o : before & ~(1 << o));
		if (d->outputs[port] != before)
			decoder_say(d->dec, t, "output %d (port %d) %s %s", (addr - 1) * 4 + port + 1, port, o ? "1" : "0",
				    on ? "on" : "off");
	} else if ((b[1] & 1) && n >= 3) {     /* extended: an aspect */
		if (b[2] != d->aspect)
			decoder_say(d->dec, t, "output %d (port %d) aspect %d", (addr - 1) * 4 + port + 1, port, b[2]);
		d->aspect = b[2];
	} else {
		d->unknown++;
	}
}

static void stat_merge(stat_t *to, const stat_t *from)
{
	if (!from->n)
		return;
	if (!to->n || from->min < to->min)
		to->min = from->min;
	if (!to->n || from->max > to->max)
		to->max = from->max;
	to->sum += from->sum;
	to->n += from->n;
}

static void packet(dcc_t *d, double t)
{
	int n = d->nbytes;
	const uint8_t *b = d->bytes;
	uint8_t x = 0;
	for (int i = 0; i < n; i++)
		x ^= b[i];
	if (n < 3) {
		d->too_short++;
		return;
	}
	if (x) {
		d->bad_xor++;
		return;
	}
	stat_add(&d->pre, d->preamble);
	stat_merge(&d->h1, &d->ph1);
	stat_merge(&d->h0, &d->ph0);
	n--;                                     /* without the XOR byte */
	d->packets++;
	if (d->log_packets) {
		char hex[3 * MAX_BYTES + 1] = "";
		for (int i = 0; i < n; i++)
			snprintf(hex + 3 * i, 4, " %02X", b[i]);
		decoder_say(d->dec, t, "packet%s", hex);
	}
	if (d->service) {
		service_packet(d, t, b, n);
		return;
	}
	if (b[0] == 0xFF) {
		d->idle++;
		return;
	}
	if (b[0] == 0 && b[1] == 0) {
		d->resets++;
		if (n == 2) {
			d->service = true;
			d->last_n = 0;
			decoder_say(d->dec, t, "service mode on");
			set_speed(d, t, 0, d->steps ? d->steps : 126, d->forward, false);
		}
		return;
	}
	if (b[0] >= 0x80 && b[0] <= 0xBF) {
		d->accessory_packets++;
		if (d->accessory)
			accessory_packet(d, t, b, n);
		return;
	}
	if (d->accessory)
		return;
	int addr, skip;
	if (b[0] == 0) {
		addr = 0, skip = 1;
		d->broadcast++;
	} else if (b[0] <= 127) {
		addr = b[0], skip = 1;
	} else if (b[0] <= 231) {
		addr = (b[0] & 0x3F) << 8 | b[1], skip = 2;
	} else {
		d->other++;
		return;
	}
	if (addr && addr < 10240)
		d->per_address[addr]++;
	bool mine = addr == address(d) && ((skip == 2) == !!(d->cv[29] & 0x20));
	if (!mine && addr)
		return;
	bool again = false;
	if (mine) {
		d->own++;
		if (d->last_own >= 0)
			stat_add(&d->interval, (t - d->last_own) * 1e3);
		d->last_own = t;
		again = repeated(d, b, n);
	}
	loco_instruction(d, t, b + skip, n - skip, !addr, again);
}

/* ---- the bit receiver ---- */

static void restart(dcc_t *d)
{
	d->state = HUNT;
	d->half = -1;
	memset(&d->ph1, 0, sizeof d->ph1);
	memset(&d->ph0, 0, sizeof d->ph0);
}

static void bit(dcc_t *d, double t, int v)
{
	switch (d->state) {
	case HUNT:
		break;                           /* the preamble counts halves */
	case START:
		break;
	case DATA:
		d->byte = (uint8_t)(d->byte << 1 | v);
		if (++d->nbits == 8) {
			if (d->nbytes < MAX_BYTES)
				d->bytes[d->nbytes] = d->byte;
			d->nbytes++;
			d->nbits = 0;
			d->state = END;
		}
		break;
	case END:
		if (!v) {
			d->state = DATA;
			d->byte = 0;
			break;
		}
		if (d->nbytes <= MAX_BYTES)
			packet(d, t);
		else
			d->other++;
		restart(d);
		d->ones = 2;                     /* the end bit begins the next preamble */
		break;
	}
}

#define TIME_EPS_US 0.01                  /* edge times in seconds carry rounding */

static void half_bit(dcc_t *d, double t, double us)
{
	int v = us >= BIT1_MIN_US - TIME_EPS_US && us <= BIT1_MAX_US + TIME_EPS_US ? 1
	      : us >= BIT0_MIN_US - TIME_EPS_US && us <= BIT0_MAX_US + TIME_EPS_US ? 0 : -1;
	if (v < 0) {
		if (d->state != HUNT)
			d->other++;
		d->ones = 0;
		restart(d);
		return;
	}
	stat_add(v ? &d->ph1 : &d->ph0, us);
	if (d->state == HUNT) {
		if (v) {
			d->ones++;
			return;
		}
		if (d->ones >= 2 * MIN_PREAMBLE) {
			d->preamble = d->ones / 2;
			d->state = START;            /* the first half of the start bit */
		} else {
			restart(d);
		}
		d->ones = 0;
		return;
	}
	if (d->state == START) {
		if (v) {
			d->ones = 1;
			restart(d);
			return;
		}
		d->state = DATA;
		d->nbits = d->nbytes = 0;
		d->byte = 0;
		d->half = -1;
		return;
	}
	if (d->half < 0) {
		d->half = v;
		d->half_us = us;
		return;
	}
	int first = d->half;
	d->half = -1;
	if (first != v) {
		d->other++;
		d->ones = v;
		restart(d);
		return;
	}
	if (v)
		stat_add(&d->asym1, us > d->half_us ? us - d->half_us : d->half_us - us);
	bit(d, t, v);
}

static void dcc_track(decoder_t *dec, double t, int pol)
{
	dcc_t *d = dec->impl;
	if (pol == d->pol)
		return;
	double us = (t - d->t_last) * 1e6;   /* how long the level that ends now lasted */
	if (d->pol != 0 && pol != 0)
		half_bit(d, t, us);
	else {
		d->ones = 0;
		restart(d);
	}
	if (d->pol == 0 && us > POWER_OFF_US) {
		stat_add(&d->off, us / 1e3);
		decoder_say(d->dec, t, "track voltage on after %.1f ms off", us / 1e3);
	}
	d->pol = pol;
	d->t_last = t;
}

static void dcc_status(const decoder_t *dec, char *buf, size_t size)
{
	const dcc_t *d = dec->impl;
	if (d->accessory) {
		snprintf(buf, size, "%s  outputs %d%d %d%d %d%d %d%d  (%lu packets)%s%s", dec->name,
			d->outputs[0] & 1, d->outputs[0] >> 1, d->outputs[1] & 1, d->outputs[1] >> 1,
			d->outputs[2] & 1, d->outputs[2] >> 1, d->outputs[3] & 1, d->outputs[3] >> 1, d->own,
			d->service ? "  service mode" : "", d->pol ? "" : "  no track voltage");
		return;
	}
	char f[30];
	for (int i = 0; i <= 12; i++)
		f[i] = d->f >> i & 1 ? '1' : '0';
	f[13] = 0;
	snprintf(buf, size, "%s  speed %s%d/%d%s  F0-F12 %s  (%lu packets)%s%s", dec->name, d->forward ? "+" : "-",
		d->speed, d->steps ? d->steps : 126, d->estop ? " estop" : "", f, d->own,
		d->service ? "  service mode" : "", d->pol ? "" : "  no track voltage");
}

static void dcc_report(decoder_t *dec, double t, FILE *out)
{
	dcc_t *d = dec->impl;
	char line[200];
	dcc_status(dec, line, sizeof line);
	fprintf(out, "%10.6f %s\n", t, line);
	fprintf(out, "%s: DCC packets %lu (bad checksum %lu, too short %lu), for this decoder %lu\n", dec->name,
		d->packets, d->bad_xor, d->too_short, d->own);
	fprintf(out, "  idle %lu, reset %lu, broadcast %lu, accessory %lu, service mode %lu, acknowledgements %lu\n",
		d->idle, d->resets, d->broadcast, d->accessory_packets, d->service_packets, d->acks);
	fprintf(out, "  not DCC (other signal) %lu, unknown instructions %lu\n", d->other, d->unknown);
	stat_print(out, "half bit of a 1", &d->h1, "us");
	stat_print(out, "half bit of a 0", &d->h0, "us");
	stat_print(out, "halves of a 1 differ", &d->asym1, "us");
	stat_print(out, "preamble", &d->pre, "bits");
	stat_print(out, "packet interval", &d->interval, "ms");
	stat_print(out, "track voltage off", &d->off, "ms");
	if (!d->accessory) {
		fprintf(out, "  packets by locomotive address:");
		for (int a = 1; a < 10240; a++)
			if (d->per_address[a])
				fprintf(out, " %d:%lu", a, d->per_address[a]);
		fputc('\n', out);
	}
}

static const decoder_ops_t dcc_ops = { dcc_track, dcc_status, dcc_report, dcc_free };
