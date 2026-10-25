#ifndef LCD_H
#define LCD_H

#include "hcs12.h"

/*==========================================================================
 * The LCD: a PT6547 driver (up to 304 segments, 1/4 duty) on SPI0, strobe on PM3.
 *
 * lcd_buf (0x1108) holds the screen as the PT6547 command stream, [strobe, data] byte
 * pairs; the SPI0 interrupt (isr_spi0_lcd, 0xC23E) sends it round and round. The PT6547
 * takes data LSB first while the HCS12 SPI sends MSB first, so the data bytes are stored
 * bit-reversed.
 *
 * The routines are the original's assembly (build/asm/siggen.s, build/asm/hal.s).
 *========================================================================*/

void lcd_init(void);                /* 0xC268: SPI0 and port set-up, lcd_buf_load, start */
void lcd_buf_load(void);            /* 0xC28C: lcd_buf from the image at 0xEE07 */
void lcd_buf_init(void);            /* 0xE046: lcd_buf from the image at 0xF5E8 */
void lcd_pump_start(void);          /* 0xE058: one isr_spi0_lcd step, called directly */
void lcd_pump_all(void);            /* 0xE061: the whole of lcd_buf, without interrupts */

/* The text into the character segments from column offset (negative: further right);
 * blink_phase: ms_tick bits 8-9 */
void lcd_render_text(const u8 *text, u16 blink_phase, s16 offset); /* 0xC29E */
/* the speed bar: bar segments (negative: reverse); mode 0 off, 1 with the direction arrows,
 * 2 without */
void lcd_set_bar(s8 bar, u8 mode);                               /* 0xC39F */
/* clear the icon segments listed from 0xEE5F up to end */
void lcd_clear_segments(const u8 *end);                          /* 0xC44A */
/* Columns the text in buf takes on the display, up to max; alternative glyphs when alt */
u16  lcd_text_width(u8 *buf, u8 alt, u8 max);                    /* 0xC465 */

#endif /* LCD_H */
