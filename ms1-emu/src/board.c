/* MS1 board model.
 *
 * Wiring, from the firmware's use of the pins:
 * - buttons: PORTA bits 0-7 and PORTB bits 0-4, active low (gpio_read_porta_active_low,
 *   gpio_read_portb_active_low, button_scan);
 * - rotary encoder: PTT bits 6 (A) and 7 (B), active low (encoder_sample);
 * - track output: PT3, PT4, PT5 (isr_timer_ch4_signal_gen, signal_gen_init); the track voltage
 *   is positive while PT4 is high and PT5 low, negative the other way round, 0 while both are
 *   equal;
 * - track feedback: PT0 edges (input capture 0), sampled data on PTT bit 1 and PORTB bit 5
 *   (isr_feedback_capture);
 * - LCD driver on SPI0 with CE on PM3 (isr_spi0_lcd): a Sanyo CCB-style frame - an
 *   address byte while CE is low, then a 10-byte data block while CE is high; the four blocks
 *   end in block codes 0-3. Bytes arrive MSB first, the driver takes them LSB first;
 * - analog: ATD0 channel 0 track voltage, channel 1 decoder voltage
 *   (adc_read_and_scale_voltages), channel 2 sampled by the track capture.
 *
 * The LCD is rendered with the firmware's own glass map: the 38x5 dot matrix at 0xF46C (per
 * column, five (display_buffer offset, mask) pairs, top row first) and the 14-segment speed bar
 * at 0xF450, both indexing the 88-byte (strobe, data) transmit buffer the frame is built from.
 * Those addresses, the font's and the icon table's are application version 4011's; version
 * 4010 (the word at 0xF73C) has them elsewhere (rom_tables). Its PB6 is an over-current input
 * (active low), a button here (pb6). */
#include "board.h"

#include <string.h>

/* mfx feedback receiver: an RDS demodulator for the 52.63 kHz carrier. Its bit clock RDCL
 * (PT0, input capture 0) runs free at 1096.5 Hz; the firmware samples RDDA (PTT 1, data) and
 * QUAL (PB5, quality) at rising edges for a data answer, and ATD0 channel 2 (the carrier
 * level) at both edges for a 1-bit answer. A decoder's data bit re-phases RDCL: a rising edge
 * with the bit on RDDA. */
#define RDS_HALF_S 456e-6
#define RDS_CARRIER_LEVEL 0x3C0     /* ATD0 channel 2 while a decoder sends the carrier */

static void reschedule(ms1_board_t *b);

#define CCB_ADDRESS 0x45          /* the address byte as the driver reads it (LSB first) */

/* The firmware tables the LCD is read back with, per application version (the word at 0xF73C);
 * an unknown version gets 4011's. */
static const struct rom_tables {
	uint16_t version;
	uint16_t matrix;              /* the glass map of the dot matrix */
	uint16_t bar;                 /* the speed bar's segments */
	uint16_t font;                /* glyph c at font + 8c: width (low nibble), then column bytes */
	uint16_t icons;               /* icon segments: (transmit-buffer offset, mask) per icon */
} rom_tables[] = {
	{ 0x4011, 0xF46C, 0xF450, 0xEF05, 0xEE5F },
	{ 0x4010, 0xF2F7, 0xF2DB, 0xED90, 0xECEA },
};

static const struct rom_tables *tables(const ms1_board_t *b)
{
	uint16_t version = (uint16_t)(mcu_peek(b->m, 0xF73C) << 8 | mcu_peek(b->m, 0xF73D));
	for (size_t i = 0; i < sizeof rom_tables / sizeof rom_tables[0]; i++)
		if (rom_tables[i].version == version)
			return &rom_tables[i];
	return &rom_tables[0];
}

#define GLASS_MATRIX (tables(b)->matrix)
#define GLASS_BAR (tables(b)->bar)
#define FONT (tables(b)->font)
#define ICON_TABLE (tables(b)->icons)
#define ICON_COUNT 83

/* Icon names, identified on MS1 hardware by the ms1-firmware project (src/include/lcd_icons.h):
 * entry n of ICON_TABLE, the table the firmware indexes from lcd_encode_char + 0x43. Unused
 * entries (mask 0) have no name. */
static const char *const icon_name[ICON_COUNT] = {
	[0] = "F1-light1", [1] = "F1-light1*", [2] = "F1-bell", [3] = "F1-bell*", [4] = "F1-turtle", [5] = "F1-turtle*",
	[8] = "F2-speaker", [9] = "F2-speaker*", [10] = "F2-ramp", [11] = "F2-ramp*",
	[16] = "F3-speaker", [17] = "F3-speaker*", [18] = "F3-turtle", [19] = "F3-turtle*", [20] = "F3-function", [21] = "F3-function*",
	[24] = "F4-music", [25] = "F4-music*", [26] = "F4-ramp", [27] = "F4-ramp*", [28] = "F4-light", [29] = "F4-light*",
	[32] = "F5-horn", [33] = "F5-horn*", [34] = "F5-light", [35] = "F5-light*", [36] = "F5-function", [37] = "F5-function*",
	[40] = "F6-horn", [41] = "F6-horn*", [42] = "F6-connect", [43] = "F6-connect*",
	[48] = "F7-steam", [49] = "F7-steam*", [50] = "F7-hook", [51] = "F7-hook*", [52] = "F7-bell", [53] = "F7-bell*",
	[56] = "F8-light2", [57] = "F8-light2*", [58] = "F8-light", [59] = "F8-light*", [60] = "F8-music", [61] = "F8-music*",
	[64] = "light", [65] = "light*",
	[72] = "DCC", [73] = "SEIN", [74] = "controller-not-working", [75] = "steam-loco", [76] = "electric-loco",
	[77] = "diesel-loco", [78] = "mfx", [79] = "MENU", [80] = "short-circuit", [81] = "warning", [82] = "STOP",
};

/* Segments outside the tables, set by lcd_set_bar (direction arrows and markings). */
static const struct { uint8_t offset, mask; const char *name; } extra_seg[] = {
	{ 0x0B, 0x10, "arrow-left" }, { 0x1B, 0x20, "arrow-right" }, { 0x0D, 0x02, "markings" },
};

static uint8_t rev8(uint8_t v)
{
	v = (uint8_t)((v & 0xF0) >> 4 | (v & 0x0F) << 4);
	v = (uint8_t)((v & 0xCC) >> 2 | (v & 0x33) << 2);
	return (uint8_t)((v & 0xAA) >> 1 | (v & 0x55) << 1);
}

/* ---- pins ---- */

static uint8_t pins_in(void *ctx, int port)
{
	ms1_board_t *b = ctx;
	switch (port) {
	case PORT_A: return b->porta;
	case PORT_B: return b->portb;
	case PORT_T: return b->ptt;
	default: return 0xFF;
	}
}

static void pins_out(void *ctx, int port, uint8_t level, uint8_t driven)
{
	ms1_board_t *b = ctx;
	if (port == PORT_T) {
		uint8_t t = (uint8_t)(level & driven & 0x38);
		uint8_t ch = t ^ b->track_level;
		for (int i = 0; i < 3; i++)
			if (ch >> (3 + i) & 1)
				b->track_edges[i]++;
		if (ch && b->track_log)
			fprintf(b->track_log, "%.7f %d %d %d\n", mcu_seconds(b->m, b->m->now), t >> 3 & 1, t >> 4 & 1, t >> 5 & 1);
		b->track_level = t;
		if (b->rds_next == UINT64_MAX) {
			/* start the receiver's clock here: mcu_reset clears the scheduled events */
			b->rds_next = b->m->now + mcu_ticks(b->m, RDS_HALF_S);
			reschedule(b);
		}
		int pol = (t & 0x30) == 0x10 ? 1 : (t & 0x30) == 0x20 ? -1 : 0;
		if (pol != b->track_pol) {
			b->track_pol = pol;
			for (int i = 0; i < b->ndecoders; i++)
				decoder_track(b->decoders[i], mcu_seconds(b->m, b->m->now), pol);
		}
	}
}

/* ---- LCD driver ---- */

static void lcd_latch(ms1_board_t *b)
{
	if (b->lcd_n == 0)
		return;
	if (b->lcd_addr == CCB_ADDRESS && b->lcd_n == LCD_BLOCK_BYTES) {
		int dd = b->lcd_rx[LCD_BLOCK_BYTES - 1] & 3;   /* last two bits on the wire */
		memcpy(b->lcd[dd], b->lcd_rx, LCD_BLOCK_BYTES);
		b->lcd_frames++;
	} else {
		b->lcd_bad++;
	}
	b->lcd_n = 0;
}

static uint8_t spi0_xfer(void *ctx, uint8_t out)
{
	ms1_board_t *b = ctx;
	bool ce = mcu_port_pins(b->m, PORT_M) & 0x08;
	if (!ce) {
		if (b->lcd_ce)
			lcd_latch(b);                         /* CE fell: the block is taken */
		b->lcd_addr = rev8(out);
	} else if (b->lcd_n < (int)sizeof b->lcd_rx) {
		b->lcd_rx[b->lcd_n++] = out;
	}
	b->lcd_ce = ce;
	return 0xFF;
}

/* Is the segment at transmit-buffer offset/mask lit on the glass? */
static bool lit(ms1_board_t *b, uint8_t offset, uint8_t mask)
{
	int pair = offset >> 1;
	if (!(offset & 1))
		return false;                             /* strobe bytes carry no segments */
	int block = pair / (LCD_BLOCK_BYTES + 1), i = pair % (LCD_BLOCK_BYTES + 1) - 1;
	if (block >= LCD_BLOCKS || i < 0)
		return false;
	return b->lcd[block][i] & mask;
}

bool board_lcd_pixel(ms1_board_t *b, int row, int col)
{
	uint16_t e = (uint16_t)(GLASS_MATRIX + col * 10 + row * 2);
	return lit(b, mcu_peek(b->m, e), mcu_peek(b->m, (uint16_t)(e + 1)));
}

bool board_lcd_bar(ms1_board_t *b, int i)
{
	uint16_t e = (uint16_t)(GLASS_BAR + i * 2);
	return lit(b, mcu_peek(b->m, e), mcu_peek(b->m, (uint16_t)(e + 1)));
}

/* What a glyph reads as: ASCII for 0x21-0x7E except the slots the font reuses, UTF-8 for the
 * umlauts, and the condensed letters the firmware substitutes in words too wide for the glass. */
static const char *glyph_label(int ch)
{
	static char ascii[2];
	switch (ch) {
	case 0x11: case 0x18: return "M";
	case 0x12: case 0x19: return "N";
	case 0x13: case 0x1A: return "W";
	case 0x10: return "L";
	case 0x7A: return "K";
	case 0x1D: return "E";
	case 0x1E: return "F";
	case 0x61: return "\u00c4";
	case 0x6F: return "\u00d6";
	case 0x75: return "\u00dc";
	case 0x64: return "\u00df";
	}
	if (ch < 0x21 || ch > 0x7E)
		return 0;
	ascii[0] = (char)ch;
	return ascii;
}

/* Read the 38x5 matrix back as text with the firmware's font (what lcd_render_text
 * draws): glyphs are ASCII-indexed, each column byte holds rows 0-4 in bits 7-3 and a glyph's
 * width includes its trailing blank column. Leading and trailing blank columns are dropped,
 * longer blank runs inside become spaces, an unmatched column becomes '?'. The condensed C
 * has the pixels of '(': a '(' between two letters reads as C. */
void board_lcd_text(ms1_board_t *b, char *out, size_t size)
{
	uint8_t cols[LCD_COLS];
	for (int c = 0; c < LCD_COLS; c++) {
		cols[c] = 0;
		for (int r = 0; r < LCD_ROWS; r++)
			if (board_lcd_pixel(b, r, c))
				cols[c] |= (uint8_t)(0x80 >> r);
	}
	int first = 0, last = LCD_COLS - 1;
	while (first < LCD_COLS && !cols[first])
		first++;
	while (last >= first && !cols[last])
		last--;
	size_t n = 0;
	for (int p = first; p <= last && n + 1 < size;) {
		if (!cols[p]) {                   /* blank run beyond a glyph's own spacing */
			int k = 0;
			while (p + k <= last && !cols[p + k])
				k++;
			for (int i = 0; i < (k + 2) / 4 && n + 1 < size; i++)
				out[n++] = ' ';
			p += k;
			continue;
		}
		int best = 0, best_w = 0;
		for (int ch = 1; ch < 0x100; ch++) {
			uint16_t g = (uint16_t)(FONT + 8 * ch);
			int w = mcu_peek(b->m, g) & 0x0F;
			if (w <= best_w || w > 7 || !glyph_label(ch))
				continue;
			bool ok = true;
			for (int i = 0; i < w && ok; i++) {
				uint8_t want = mcu_peek(b->m, (uint16_t)(g + 1 + i)) & 0xF8;
				uint8_t have = p + i <= last ? cols[p + i] : 0;
				ok = want == have;
			}
			if (ok) {
				best = ch;
				best_w = w;
			}
		}
		if (!best_w) {
			out[n++] = '?';
			p++;
			continue;
		}
		const char *label = glyph_label(best);
		size_t len = strlen(label);
		if (n + len + 1 > size)
			break;
		memcpy(out + n, label, len);
		n += len;
		p += best_w;
	}
	out[n] = 0;
	for (size_t i = 1; i + 1 < n; i++) {
		bool before = (out[i - 1] >= 'A' && out[i - 1] <= 'Z') || (unsigned char)out[i - 1] >= 0x80;
		bool after = (out[i + 1] >= 'A' && out[i + 1] <= 'Z') || (unsigned char)out[i + 1] >= 0x80;
		if (out[i] == '(' && before && after)
			out[i] = 'C';
	}
}

void board_lcd_icons(ms1_board_t *b, char *out, size_t size)
{
	size_t n = 0;
	out[0] = 0;
	for (int i = 0; i < ICON_COUNT; i++) {
		uint8_t off = mcu_peek(b->m, (uint16_t)(ICON_TABLE + 2 * i));
		uint8_t mask = mcu_peek(b->m, (uint16_t)(ICON_TABLE + 2 * i + 1));
		if (mask && lit(b, off, mask)) {
			int k = icon_name[i] ? snprintf(out + n, size - n, "%s%s", n ? " " : "", icon_name[i])
					     : snprintf(out + n, size - n, "%sA%d", n ? " " : "", i);
			if (k < 0 || (size_t)k >= size - n)
				return;
			n += (size_t)k;
		}
	}
	for (size_t i = 0; i < sizeof extra_seg / sizeof extra_seg[0]; i++)
		if (lit(b, extra_seg[i].offset, extra_seg[i].mask)) {
			int k = snprintf(out + n, size - n, "%s%s", n ? " " : "", extra_seg[i].name);
			if (k < 0 || (size_t)k >= size - n)
				return;
			n += (size_t)k;
		}
}

/* Mark a (transmit-buffer offset, mask) segment as accounted for. */
static void claim(char used[LCD_BLOCKS][LCD_BLOCK_BYTES], uint8_t off, uint8_t mask)
{
	int pair = off >> 1, blk = pair / (LCD_BLOCK_BYTES + 1), i = pair % (LCD_BLOCK_BYTES + 1) - 1;
	if ((off & 1) && blk < LCD_BLOCKS && i >= 0)
		used[blk][i] |= (char)mask;
}

void board_lcd_print(ms1_board_t *b, FILE *out)
{
	mcu_t *m = b->m;
	char used[LCD_BLOCKS][LCD_BLOCK_BYTES];
	memset(used, 0, sizeof used);
	char text[64];
	board_lcd_text(b, text, sizeof text);
	fprintf(out, "LCD \"%s\" (%u blocks latched%s):\n", text, b->lcd_frames, b->lcd_bad ? ", some frames malformed" : "");
	for (int row = 0; row < LCD_ROWS; row++) {
		fputs("  |", out);
		for (int col = 0; col < LCD_COLS; col++) {
			uint16_t e = (uint16_t)(GLASS_MATRIX + col * 10 + row * 2);
			uint8_t off = mcu_peek(m, e), mask = mcu_peek(m, (uint16_t)(e + 1));
			fputc(lit(b, off, mask) ? '#' : '.', out);
		}
		fputs("|\n", out);
	}
	fputs("  speed |", out);
	for (int i = 0; i < LCD_BAR; i++) {
		uint16_t e = (uint16_t)(GLASS_BAR + i * 2);
		fputc(lit(b, mcu_peek(m, e), mcu_peek(m, (uint16_t)(e + 1))) ? '=' : ' ', out);
	}
	fputs("|\n", out);
	char icons[512];
	board_lcd_icons(b, icons, sizeof icons);
	fprintf(out, "  icons: %s\n", icons[0] ? icons : "none");
	/* anything lit outside the matrix, the bar and the icons */
	for (int col = 0; col < LCD_COLS; col++)
		for (int row = 0; row < LCD_ROWS; row++) {
			uint16_t e = (uint16_t)(GLASS_MATRIX + col * 10 + row * 2);
			claim(used, mcu_peek(m, e), mcu_peek(m, (uint16_t)(e + 1)));
		}
	for (int s = 0; s < LCD_BAR; s++) {
		uint16_t e = (uint16_t)(GLASS_BAR + s * 2);
		claim(used, mcu_peek(m, e), mcu_peek(m, (uint16_t)(e + 1)));
	}
	for (int i = 0; i < ICON_COUNT; i++)
		claim(used, mcu_peek(m, (uint16_t)(ICON_TABLE + 2 * i)), mcu_peek(m, (uint16_t)(ICON_TABLE + 2 * i + 1)));
	for (size_t i = 0; i < sizeof extra_seg / sizeof extra_seg[0]; i++)
		claim(used, extra_seg[i].offset, extra_seg[i].mask);
	fputs("  other segments lit (block.byte.mask):", out);
	int n = 0;
	for (int blk = 0; blk < LCD_BLOCKS; blk++)
		for (int i = 0; i < LCD_BLOCK_BYTES - 1; i++) {
			uint8_t rest = (uint8_t)(b->lcd[blk][i] & ~used[blk][i]);
			if (rest) {
				fprintf(out, " %d.%d.%02X", blk, i, rest);
				n++;
			}
		}
	fputs(n ? "\n" : " none\n", out);
}

/* ---- inputs ---- */

void board_button(ms1_board_t *b, int index, bool pressed)
{
	if (index < 8)
		b->porta = pressed ? (uint8_t)(b->porta & ~(1 << index)) : (uint8_t)(b->porta | 1 << index);
	else
		b->portb = pressed ? (uint8_t)(b->portb & ~(1 << (index - 8))) : (uint8_t)(b->portb | 1 << (index - 8));
}

/* Quadrature sequence of (A, B) levels, active low: phase 0 = both released (high). */
static const uint8_t enc_seq[4] = { 0xC0, 0x80, 0x00, 0x40 };

/* Time between two encoder transitions (a detent is two): a quick hand turn. The firmware
 * takes a line as released only after 12 equal samples, two per main-loop pass, so
 * transitions much closer than that are lost or counted as jitter, in numbers that depend on
 * the main loop's timing. */
#define ENC_TRANSITION_S 0.04

static void enc_apply(ms1_board_t *b)
{
	b->ptt = (uint8_t)((b->ptt & 0x3F) | enc_seq[b->enc_phase & 3]);
	ect_pins_changed(b->m, mcu_port_pins(b->m, PORT_T));
}

static void reschedule(ms1_board_t *b)
{
	uint64_t next = b->enc_next < b->poll_next ? b->enc_next : b->poll_next;
	mcu_schedule(b->m, EV_BOARD, b->rds_next < next ? b->rds_next : next);
}

/* ---- mfx feedback ---- */

static void rds_clock(ms1_board_t *b, bool level)
{
	b->rds_clk = level;
	b->ptt = (uint8_t)((b->ptt & ~1) | level);
	ect_pins_changed(b->m, mcu_port_pins(b->m, PORT_T));
}

static void rds_carrier(void *ctx, bool on)
{
	ms1_board_t *b = ctx;
	b->rds_carriers += on ? 1 : -1;
	b->portb = (uint8_t)(b->rds_carriers > 0 ? b->portb | 0x20 : b->portb & ~0x20);
}

static void rds_bit(void *ctx, int bit)
{
	ms1_board_t *b = ctx;
	b->ptt = (uint8_t)((b->ptt & ~2) | (bit ? 2 : 0));
	if (b->rds_clk)
		rds_clock(b, false);
	rds_clock(b, true);
	b->rds_hold = true;
	b->rds_next = b->m->now + mcu_ticks(b->m, RDS_HALF_S);
	reschedule(b);
}

decoder_feedback_t board_feedback(ms1_board_t *b)
{
	return (decoder_feedback_t){ .ctx = b, .carrier = rds_carrier, .bit = rds_bit };
}

static void poll_bus(ms1_board_t *b)
{
	uint32_t id;
	bool ext;
	uint8_t dlc, data[8], idr[4];
	while (canbus_recv(b->bus, &id, &ext, &dlc, data)) {
		canbus_id_to_idr(id, ext, idr);
		board_can_receive(b, idr, dlc, data);
	}
}

static void board_event(void *ctx)
{
	ms1_board_t *b = ctx;
	uint64_t now = b->m->now;
	if (b->enc_next <= now) {
		b->enc_next = UINT64_MAX;
		if (b->enc_pending) {
			int dir = b->enc_pending > 0 ? 1 : -1;
			b->enc_phase += dir;
			b->enc_pending -= dir;
			enc_apply(b);
		}
		if (b->enc_pending)
			b->enc_next = now + mcu_ticks(b->m, ENC_TRANSITION_S);
	}
	if (b->poll_next <= now) {
		poll_bus(b);
		b->poll_next = now + mcu_ticks(b->m, 0.0005);
	}
	if (b->rds_next <= now) {
		rds_clock(b, !b->rds_clk);
		/* after a decoder's bit its next one, about a bit time later, makes the rising edge */
		b->rds_next = now + mcu_ticks(b->m, !b->rds_clk && b->rds_hold ? 2 * RDS_HALF_S : RDS_HALF_S);
		b->rds_hold = false;
	}
	reschedule(b);
}

void board_encoder_step(ms1_board_t *b, int dir)
{
	b->enc_pending += 2 * dir;
	if (b->enc_next == UINT64_MAX) {
		b->enc_next = b->m->now + mcu_ticks(b->m, ENC_TRANSITION_S);
		reschedule(b);
	}
}

void board_attach_decoder(ms1_board_t *b, decoder_t *d)
{
	if (b->ndecoders < (int)(sizeof b->decoders / sizeof b->decoders[0]))
		b->decoders[b->ndecoders++] = d;
}

void board_can_plug(ms1_board_t *b, bool plugged)
{
	b->can_unplugged = !plugged;
}

void board_attach_bus(ms1_board_t *b, canbus_t *bus)
{
	b->bus = bus;
	b->poll_next = b->m->now;
	reschedule(b);
}

/* ---- analog ---- */

static uint16_t atd0_input(void *ctx, int ch)
{
	ms1_board_t *b = ctx;
	if ((ch & 7) == 2 && b->rds_carriers > 0)
		return RDS_CARRIER_LEVEL;
	return b->atd[ch & 7];
}

/* ---- CAN ---- */

static void can_print(FILE *f, double t, const char *dir, const uint8_t idr[4], uint8_t dlc, const uint8_t *data)
{
	bool ext;
	uint32_t id = canbus_idr_to_id(idr, &ext);
	fprintf(f, "%10.6f %s %s%08X [%u]", t, dir, ext ? "" : "std ", id, dlc);
	for (int i = 0; i < dlc && i < 8; i++)
		fprintf(f, " %02X", data[i]);
	fputc('\n', f);
}

static void can0_tx(void *ctx, const uint8_t idr[4], uint8_t dlc, const uint8_t *data)
{
	ms1_board_t *b = ctx;
	b->can_tx++;
	if (b->can_log)
		can_print(b->can_log, mcu_seconds(b->m, b->m->now), b->can_unplugged ? "tx-lost" : "TX", idr, dlc, data);
	if (b->can_unplugged)
		return;
	if (b->bus) {
		bool ext;
		uint32_t id = canbus_idr_to_id(idr, &ext);
		canbus_send(b->bus, id, ext, dlc, data);
	}
	if (b->peer)
		board_can_receive(b->peer, idr, dlc, data);
}

bool board_can_receive(ms1_board_t *b, const uint8_t idr[4], uint8_t dlc, const uint8_t *data)
{
	if (b->can_unplugged)
		return false;
	bool ok = mscan_receive(b->m, idr, dlc, data);
	b->can_rx++;
	if (b->can_log)
		can_print(b->can_log, mcu_seconds(b->m, b->m->now), ok ? "RX" : "rx-dropped", idr, dlc, data);
	return ok;
}

void board_init(ms1_board_t *b, mcu_t *m, FILE *can_log)
{
	memset(b, 0, sizeof *b);
	b->m = m;
	b->can_log = can_log;
	b->porta = 0xFF;
	b->portb = 0xDF;                /* PB5: no feedback carrier */
	b->ptt = 0xFF;
	b->rds_clk = true;
	b->enc_next = b->poll_next = b->rds_next = UINT64_MAX;
	for (int i = 0; i < 8; i++)
		b->atd[i] = 0x200;
	m->board = (board_if_t){
		.ctx = b,
		.pins_in = pins_in,
		.pins_out = pins_out,
		.spi0_xfer = spi0_xfer,
		.can0_tx = can0_tx,
		.atd0_input = atd0_input,
		.event = board_event,
	};
}
