/* Tests of the DCC decoders (src/decoder_dcc.c) on synthetic track signals: packets built here
 * bit by bit, per RCN-211 and the commands of RCN-212/213/214/216, with the bit times and
 * acceptance limits of RCN-210. Run by make check. */
#define _POSIX_C_SOURCE 200809L          /* open_memstream */
#include "../src/decoder.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static double now;          /* s */
static int pol = 1;
static int acks;
static double ack_ma;

static void current(void *ctx, double ma, double seconds)
{
	(void)ctx;
	(void)seconds;
	acks++;
	ack_ma = ma;
}

static void edge(decoder_t *d, double half_us)
{
	pol = -pol;
	decoder_track(d, now, pol);
	now += half_us * 1e-6;
}

static void bit(decoder_t *d, int b, double h1, double h0)
{
	edge(d, b ? h1 : h0);
	edge(d, b ? h1 : h0);
}

/* a packet: preamble ones, then each byte after a 0, the XOR byte, the end bit */
static void packet_timed(decoder_t *d, const uint8_t *bytes, int n, int preamble, double h1, double h0, bool good_xor)
{
	uint8_t x = 0;
	for (int i = 0; i < preamble; i++)
		bit(d, 1, h1, h0);
	for (int i = 0; i <= n; i++) {
		uint8_t v = i < n ? bytes[i] : (uint8_t)(good_xor ? x : x ^ 1);
		if (i < n)
			x ^= bytes[i];
		bit(d, 0, h1, h0);
		for (int k = 7; k >= 0; k--)
			bit(d, v >> k & 1, h1, h0);
	}
	bit(d, 1, h1, h0);
	bit(d, 1, h1, h0);      /* the next preamble's first bit ends the end bit's last half */
}

static void packet(decoder_t *d, const uint8_t *bytes, int n)
{
	packet_timed(d, bytes, n, 16, 58, 100, true);
}

#define PACKET(d, ...) do { const uint8_t p_[] = { __VA_ARGS__ }; packet(d, p_, (int)sizeof p_); } while (0)

/* the decoder's events so far contain want (or, with absent, do not) */
static void expect(const char *what, char *log, bool absent, const char *want)
{
	bool found = strstr(log, want) != NULL;
	if (found == absent) {
		printf("FAIL %s: %s '%s' in:\n%s\n", what, absent ? "unexpected" : "missing", want, log);
		failures++;
	}
}

typedef struct {
	decoder_t *d;
	FILE *f;
	char *buf;
	size_t size;
} dut_t;

/* the stream writes its buffer's address into u: u must outlive it */
static void make(dut_t *u, const char *spec)
{
	char err[200];
	u->f = open_memstream(&u->buf, &u->size);
	decoder_feedback_t fb = { .ctx = NULL, .current = current };
	u->d = decoder_create(spec, u->f, &fb, err, sizeof err);
	if (!u->d) {
		printf("FAIL create %s: %s\n", spec, err);
		exit(1);
	}
	now = 0;
	pol = 1;
	acks = 0;
	edge(u->d, 100);
}

static char *events(dut_t *u)
{
	fflush(u->f);
	return u->buf;
}

static void done(dut_t *u)
{
	fclose(u->f);
	free(u->buf);
	decoder_free(u->d);
}

static void test_speed_functions(void)
{
	dut_t u;
	make(&u, "dcc:3");
	PACKET(u.d, 0x03, 0x3F, 0x80 | 43);                 /* 128 steps: forward, step 42 */
	PACKET(u.d, 0x04, 0x3F, 0x80 | 20);                 /* another address */
	PACKET(u.d, 0x03, 0x90 | 0x01);                     /* F0, F1 on */
	PACKET(u.d, 0x03, 0xB4);                            /* F7 on */
	PACKET(u.d, 0x03, 0xA8);                            /* F12 on */
	PACKET(u.d, 0x03, 0xDE, 0x01);                      /* F13 on */
	PACKET(u.d, 0x03, 0x3F, 0x01);                      /* emergency stop, reverse */
	PACKET(u.d, 0x03, 0x68);                            /* 28 steps: forward, step 13 */
	PACKET(u.d, 0x00, 0x3F, 0x00);                      /* broadcast: stop, reverse */
	char *log = events(&u);
	expect("128 steps", log, false, "dcc:3 speed +42/126");
	expect("other address", log, true, "speed +19/126");
	expect("F0", log, false, "dcc:3 F0 on");
	expect("F1", log, false, "dcc:3 F1 on");
	expect("F7", log, false, "dcc:3 F7 on");
	expect("F12", log, false, "dcc:3 F12 on");
	expect("F13", log, false, "dcc:3 F13 on");
	expect("emergency stop", log, false, "dcc:3 emergency stop -");
	expect("28 steps", log, false, "dcc:3 speed +13/28");
	expect("broadcast stop", log, false, "dcc:3 speed -0/126");
	done(&u);

	make(&u, "dcc:3,steps=14");
	PACKET(u.d, 0x03, 0x60 | 0x10 | 0x05);              /* 14 steps: forward, step 4, F0 on */
	log = events(&u);
	expect("14 steps", log, false, "dcc:3 speed +4/14");
	expect("14 steps F0", log, false, "dcc:3 F0 on");
	done(&u);
}

static void test_long_address(void)
{
	dut_t u;
	make(&u, "dcc:1027");
	PACKET(u.d, 0xC0 | (1027 >> 8), 1027 & 0xFF, 0x3F, 0x80 | 5);
	PACKET(u.d, 0x03, 0x3F, 0x80 | 9);                  /* short address 3: not this one */
	char *log = events(&u);
	expect("long address", log, false, "dcc:1027 speed +4/126");
	expect("long address only", log, true, "speed +8/126");
	done(&u);

	make(&u, "dcc:5,long");
	PACKET(u.d, 0x05, 0x3F, 0x80 | 9);                  /* short 5: not it */
	PACKET(u.d, 0xC0, 0x05, 0x3F, 0x80 | 3);            /* long 5 */
	log = events(&u);
	expect("long below 128", log, false, "dcc:5 speed +2/126");
	expect("long below 128 only", log, true, "speed +8/126");
	done(&u);
}

static void test_pom(void)
{
	dut_t u;
	make(&u, "dcc:3");
	PACKET(u.d, 0x03, 0xEC, 0x04, 200);                 /* write CV5 = 200, once */
	PACKET(u.d, 0x03, 0x3F, 0x80);
	char *log = events(&u);
	expect("POM once", log, true, "CV 5 = 200");
	PACKET(u.d, 0x03, 0xEC, 0x04, 200);
	PACKET(u.d, 0x03, 0xEC, 0x04, 200);                 /* twice in a row */
	log = events(&u);
	expect("POM twice", log, false, "dcc:3 CV 5 = 200 (POM)");
	PACKET(u.d, 0x03, 0xE8, 0x1C, 0xF8 | 0x02);         /* bit write: CV29 bit 2 = 1 */
	PACKET(u.d, 0x03, 0xE8, 0x1C, 0xF8 | 0x02);
	log = events(&u);
	expect("POM bit", log, false, "dcc:3 CV 29 = 6 (POM)");
	expect("POM no ack", log, true, " ack");
	done(&u);
}

static void test_service(void)
{
	dut_t u;
	make(&u, "dcc:3");
	for (int i = 0; i < 3; i++)
		PACKET(u.d, 0x00, 0x00);                        /* resets: service mode */
	for (int i = 0; i < 5; i++)
		PACKET(u.d, 0x74, 0x00, 0x03);                  /* verify CV1 = 3 */
	char *log = events(&u);
	expect("service on", log, false, "dcc:3 service mode on");
	expect("verify", log, false, "dcc:3 verify CV 1 = 3");
	if (acks != 1 || ack_ma < 60) {
		printf("FAIL verify: %d acknowledgements of %.0f mA, want 1 of >= 60\n", acks, ack_ma);
		failures++;
	}
	for (int i = 0; i < 5; i++)
		PACKET(u.d, 0x74, 0x00, 0x04);                  /* verify CV1 = 4: no */
	if (acks != 1) {
		printf("FAIL verify mismatch: acknowledged\n");
		failures++;
	}
	for (int i = 0; i < 5; i++)
		PACKET(u.d, 0x78, 0x1C, 0xE8 | 0x01);           /* verify CV29 bit 1 = 1 */
	for (int i = 0; i < 5; i++)
		PACKET(u.d, 0x7C, 0x00, 0x05);                  /* write CV1 = 5 */
	log = events(&u);
	expect("verify bit", log, false, "dcc:3 verify CV 29 bit 1 = 1");
	expect("service write", log, false, "dcc:3 CV 1 = 5 (service)");
	if (acks != 3) {
		printf("FAIL service: %d acknowledgements, want 3\n", acks);
		failures++;
	}
	PACKET(u.d, 0xFF, 0x00);                            /* idle: leaves service mode */
	PACKET(u.d, 0x05, 0x3F, 0x80 | 3);
	log = events(&u);
	expect("service off", log, false, "dcc:3 service mode off");
	expect("new address", log, false, "dcc:3 speed +2/126");
	done(&u);

	make(&u, "dcc:3,noack");
	for (int i = 0; i < 3; i++)
		PACKET(u.d, 0x00, 0x00);
	for (int i = 0; i < 5; i++)
		PACKET(u.d, 0x74, 0x00, 0x03);
	if (acks) {
		printf("FAIL noack: acknowledged\n");
		failures++;
	}
	done(&u);
}

static void test_accessory(void)
{
	dut_t u;
	make(&u, "dccacc:5");
	/* decoder 5, port 2, output 1 on: 10 000101, 1 111 1 10 1 */
	PACKET(u.d, 0x85, 0x80 | 0x70 | 0x08 | 0x04 | 0x01);
	PACKET(u.d, 0x86, 0x80 | 0x70 | 0x08 | 0x01);        /* decoder 6: not this one */
	PACKET(u.d, 0x85, 0x80 | 0x70 | 0x04 | 0x01);        /* port 2, output 1 off */
	char *log = events(&u);
	expect("accessory on", log, false, "dccacc:5 output 19 (port 2) 1 on");
	expect("accessory off", log, false, "dccacc:5 output 19 (port 2) 1 off");
	expect("other accessory", log, true, "output 21");
	done(&u);
}

static void test_signal(void)
{
	dut_t u;
	make(&u, "dcc:3");
	const uint8_t p[] = { 0x03, 0x3F, 0x80 | 43 };
	packet_timed(u.d, p, 3, 16, 52, 90, true);          /* the shortest halves accepted */
	char *log = events(&u);
	expect("short halves", log, false, "dcc:3 speed +42/126");
	done(&u);

	make(&u, "dcc:3");
	const uint8_t q[] = { 0x03, 0x3F, 0x80 | 43 };
	packet_timed(u.d, q, 3, 16, 66, 100, true);         /* a 1 half of 66 us: no DCC */
	packet_timed(u.d, q, 3, 8, 58, 100, true);          /* 8 preamble bits: too few */
	packet_timed(u.d, q, 3, 16, 58, 100, false);        /* bad checksum */
	log = events(&u);
	expect("bad packets", log, true, "speed +42/126");
	done(&u);
}

int main(void)
{
	test_speed_functions();
	test_long_address();
	test_pom();
	test_service();
	test_accessory();
	test_signal();
	if (failures) {
		printf("dcc_test: %d failures\n", failures);
		return 1;
	}
	printf("dcc_test: all passed\n");
	return 0;
}
