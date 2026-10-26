#ifndef ROM_TABLES_H
#define ROM_TABLES_H

#include "hcs12.h"

/*==========================================================================
 * The read-only tables several files share (the others are static in their user), and
 * the words of the bootloader's flash metadata the code reads by name. Words are big-endian.
 *========================================================================*/

/*--- The LCD (docs/architecture.md: Display) ---*/

/* lcd_buf images (0x58 bytes, [strobe, data] pairs): blank, loaded by lcd_buf_load;
 * start-up, most segments on, loaded by lcd_buf_init (ui/lcd_tables.c) */
#define LCD_IMAGE_SIZE      0x58
extern const u8 lcd_blank_image[LCD_IMAGE_SIZE];
extern const u8 lcd_start_image[LCD_IMAGE_SIZE];

/* An (lcd_buf offset, mask) pair per icon (indices in lcd_icons.h); read by
 * lcd_clear_segments and ui/lok_icons.c */
extern const u8 lcd_icon_segments[];

/* 8 bytes per character code: the width in the low nibble of byte 0, then the column bytes;
 * read by lcd_render_text, lcd_text_width */
extern const u8 lcd_font[];

/* The speed bar: an (lcd_buf offset, mask) pair per bar segment; read by lcd_set_bar */
extern const u8 lcd_bar_segments[];

/* The text dot matrix, 38 columns x 5 dots: per column five (lcd_buf offset, mask) pairs, one
 * per dot, bit 7 of the glyph's column byte first; read by lcd_render_text */
extern const u8 lcd_text_dots[];

/* The characters of the name editor in knob order (ui/ui_helpers.c): ui_control.c,
 * ui_helpers.c */
#define CHARSET_INDEX_MAP_SIZE      75
extern const u8 charset_index_map[CHARSET_INDEX_MAP_SIZE];

/* Per language (language_index) LANG_COUNT texts (ui/lang.c), in the display character set */
#define LANG_COUNT  33
extern const u8 *const *const lang_table[];
#define LANG(k) (lang_table[language_index][k])

/* The bootloader's flash metadata (gcc/ms1.ld; docs/bootloader.md: The metadata block) */
extern const u16 flash_uid_hi;             /* 0xFF10: this device's UID */
extern const u16 flash_uid_lo;             /* 0xFF12 */
extern const u16 flash_hw_version;         /* 0xFF18: major.minor */
extern const u16 flash_hw_code;            /* 0xFF1A: what the bootloader matches the
                                            * application version word (0xF73C) against */
extern const u16 flash_compat_flags;       /* 0xFF2C */
extern const u16 flash_adc0_scale;         /* 0xFF60 */
extern const u16 flash_adc1_scale;         /* 0xFF62 */
extern const u16 flash_adc3_scale;         /* 0xFF66 */
extern const u16 flash_adc4_scale;         /* 0xFF68: the track current sense */
extern const u16 flash_current_limit;      /* 0xFF7A: mA */

#endif /* ROM_TABLES_H */
