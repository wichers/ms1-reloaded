/*==========================================================================
 * ui/lcd.c - the display: the SPI0 interrupt that sends lcd_buf round and round, and the
 * drawing into lcd_buf (docs/architecture.md: Display)
 *
 * The glass maps the drawing reads are (lcd_buf offset, mask) pairs (src/ui/lcd_tables.c).
 *
 * Functions:
 *   isr_spi0_lcd        (0xC23E) - interrupt: the next byte of lcd_buf
 *   lcd_pump_start      (0xE058) - isr_spi0_lcd, called
 *   lcd_pump_all        (0xE061) - all of lcd_buf, a byte a millisecond
 *   lcd_buf_init        (0xE046) - lcd_buf from the start-up image
 *   lcd_init            (0xC268) - SPI0 and port set-up, blank screen, start the pump
 *   lcd_buf_load        (0xC28C) - lcd_buf from the blank image
 *   lcd_render_text     (0xC29E) - text into the dot matrix
 *   lcd_set_bar         (0xC39F) - the speed bar and its direction arrows
 *   lcd_clear_segments  (0xC44A) - icons off
 *   lcd_text_width      (0xC465) - the columns a text takes, shortened to fit
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/asm.h"
#include "../include/globals.h"
#include "../include/rom_tables.h"
#include "../include/lcd.h"
#include "../include/delay.h"

#define SEG_ON(p)   (lcd_buf[(p)[0]] |= (p)[1])
#define SEG_OFF(p)  (lcd_buf[(p)[0]] &= ~(p)[1])

#define TEXT_COLUMNS    38
#define BAR_SEGMENTS    14
#define GLYPH(c)        (&lcd_font[(u16)(c) * 8])
/* the glyph that ends a shortened text */
#define ELLIPSIS        0x1F

/* Each SPI0 transfer's end sends the next data byte of lcd_buf, the strobe (PM3) set as the
 * byte before it says. lcd_pump_start enters the handler as if its interrupt came: an RTI
 * frame of the registers below its return address. */
__asm__ (
"	.sect	.text.isr_spi0_lcd\n"
"	.globl	isr_spi0_lcd\n"
"isr_spi0_lcd:\n"
"	brclr	SPI0SR,#0x80,.Llcd_done\n"      /* SPIF */
"	ldaa	SPI0DR\n"                       /* clears SPIF */
"	ldx	lcd_pump_ptr\n"
"	ldab	1,x+\n"
"	bne	.Llcd_strobe\n"
"	bclr	PTM,#8\n"
"	bra	.Llcd_send\n"
".Llcd_strobe:\n"
"	bset	PTM,#8\n"
".Llcd_send:\n"
"	movb	1,x+,SPI0DR\n"
"	cpx	#lcd_buf+" ASM_NUM(LCD_IMAGE_SIZE) "\n"
"	bne	.Llcd_next\n"
"	ldx	#lcd_buf\n"
".Llcd_next:\n"
"	stx	lcd_pump_ptr\n"
".Llcd_done:\n"
"	rti\n"

"	.sect	.text.lcd_pump_start\n"
"	.globl	lcd_pump_start\n"
"lcd_pump_start:\n"
"	pshy\n"
"	pshx\n"
"	pshd\n"
"	pshc\n"
"	orcc	#0x50\n"
"	jmp	isr_spi0_lcd\n"
);

/* with interrupts off: what the interrupt would send */
void lcd_pump_all(void)
{
    u8 n = LCD_IMAGE_SIZE;

    do {
        lcd_pump_start();
        delay_short(1);
    } while (--n);
}

static void lcd_buf_copy(const u8 *image)
{
    const u16 *s = (const u16 *)image;
    u16 *d = (u16 *)lcd_buf;
    u8 n = LCD_IMAGE_SIZE / 2;

    do
        *d++ = *s++;
    while (--n);
}

void lcd_buf_init(void)
{
    lcd_buf_copy(lcd_start_image);
}

void lcd_init(void)
{
    MODRR |= 0x10;
    PORTE &= ~0x04;
    DDRE |= 0x04;
    PTM &= ~0x08;
    DDRM |= 0x08;
    SPI0BR = 0x77;
    SPI0CR1 = 0xDC;
    lcd_buf_load();
    SPI0DR = 0;         /* the first byte; each transfer's interrupt sends the next */
}

void lcd_buf_load(void)
{
    lcd_buf_copy(lcd_blank_image);
}

/* A glyph column into the five dots from dot, bit 7 the top */
static void lcd_draw_column(const u8 *dot, u8 col)
{
    u8 i;

    for (i = 5; i; i--) {
        if (col & 0x80)
            SEG_ON(dot);
        else
            SEG_OFF(dot);
        col <<= 1;
        dot += 2;
    }
}

/* Glyph columns from the left of the matrix: a negative offset first blanks -offset columns,
 * a positive one leaves out the text's first offset columns. After the text the matrix is
 * filled with glyph 5. */
void lcd_render_text(const u8 *text, u16 blink_phase, s16 offset)
{
    const u8 *dot = lcd_text_dots;
    const u8 *glyph;
    u8 c, w, col;

    if (offset < 0) {
        u16 n = -offset;
        u16 k = (n < TEXT_COLUMNS ? n : TEXT_COLUMNS) * 5;

        offset = 0;
        do {
            SEG_OFF(dot);
            dot += 2;
        } while (--k);
    }
    for (;;) {
        c = *text++;
        if (c == 0) {
            text--;
            c = 5;
        }
        if (c & 0x80) {
            c &= 0x7F;
            if (blink_phase == 0)
                c = GLYPH(c)[0] & 15;   /* the blank glyph of the same width */
        }
        glyph = GLYPH(c);
        w = *glyph++ & 15;
        while (w--) {
            if (dot >= lcd_text_dots + TEXT_COLUMNS * 10)
                return;
            col = *glyph++;
            if (offset != 0) {
                offset--;
                continue;
            }
            lcd_draw_column(dot, col);
            dot += 10;
        }
    }
}

void lcd_set_bar(s8 bar, u8 mode)
{
    const u8 *p;
    u8 n;

    if ((s8)mode < 1)
        lcd_buf[0x0D] &= ~0x02;
    else
        lcd_buf[0x0D] |= 0x02;
    if (mode == 1) {
        if (bar >= 0) {
            lcd_buf[0x1B] |= 0x20;
            lcd_buf[0x0B] &= ~0x10;
        } else {
            lcd_buf[0x0B] |= 0x10;
            lcd_buf[0x1B] &= ~0x20;
        }
    } else {
        lcd_buf[0x0B] &= ~0x10;
        lcd_buf[0x1B] &= ~0x20;
    }
    if (mode == 0xFF)
        bar = 0;
    if (bar >= 0) {
        n = bar < BAR_SEGMENTS ? bar : BAR_SEGMENTS;
        for (p = lcd_bar_segments; n; n--, p += 2)
            SEG_ON(p);
        for (; p != lcd_bar_segments + BAR_SEGMENTS * 2; p += 2)
            SEG_OFF(p);
    } else {
        /* from the other end: -1 lights none */
        n = ~bar;
        if (n > BAR_SEGMENTS)
            n = BAR_SEGMENTS;
        for (p = lcd_bar_segments + BAR_SEGMENTS * 2; n; n--) {
            p -= 2;
            SEG_ON(p);
        }
        while (p != lcd_bar_segments) {
            p -= 2;
            SEG_OFF(p);
        }
    }
}

void lcd_clear_segments(const u8 *end)
{
    const u8 *p = lcd_icon_segments;

    do {
        SEG_OFF(p);
        p += 2;
    } while (p != end);
}

/* Each glyph has a level (the high nibble of its byte 0) and an alternative (byte 7). While
 * the text is wider than max, the next pass, from level 6 down to 1, replaces the characters
 * of that level with their alternatives in buf. If it is still too wide then and alt is set,
 * the text is cut where it reaches max less the ellipsis and the ellipsis ends it. */
u16 lcd_text_width(u8 *buf, u8 alt, u8 max)
{
    u8 *s;
    const u8 *g;
    u8 w = 0xFF, level = 6, c;

    while (w > max) {
        s = buf;
        if (level == 0) {
            if (!alt)
                break;
            max -= GLYPH(ELLIPSIS)[0];
            w = 0;
            for (;;) {
                /* bit 7 (blinking) left out: the original reads such a character's
                 * width from past the end of the font */
                c = (GLYPH(*s++ & 0x7F)[0] & 15) + w;
                if (c > max)
                    break;
                w = c;
            }
            s[-1] = ELLIPSIS;
            s[0] = 0;
            w += GLYPH(ELLIPSIS)[0];
            break;
        }
        w = 0;
        while ((c = *s++ & 0x7F) != 0) {
            g = GLYPH(c);
            if (g[0] >> 4 == level) {
                c = g[7];
                s[-1] = c | (s[-1] & 0x80);
                g = GLYPH(c & 0x7F);
            }
            w += g[0] & 15;
        }
        level--;
    }
    return w;
}
