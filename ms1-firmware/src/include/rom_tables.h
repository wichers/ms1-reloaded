#ifndef ROM_TABLES_H
#define ROM_TABLES_H

#include "hcs12.h"

/*==========================================================================
 * The original's read-only tables: gcc/ms1.ld defines each symbol at the address given
 * here, tools/mkimage.py copies the bytes. Words are big-endian.
 *========================================================================*/

/*--- The LCD (docs/architecture.md: Display) ---*/

/* lcd_buf images (0x58 bytes, [strobe, data] pairs): blank, loaded by lcd_buf_load (0xEE07);
 * start-up, most segments on, loaded by lcd_buf_init (0xF5E8) */
#define LCD_IMAGE_SIZE      0x58
extern const u8 lcd_blank_image[LCD_IMAGE_SIZE];
extern const u8 lcd_start_image[LCD_IMAGE_SIZE];

/* An (lcd_buf offset, mask) pair per icon, from 0xEE5F (indices in lcd_icons.h); read by
 * lcd_clear_segments and ui/lok_icons.c */
extern const u8 lcd_icon_segments[];

/* 8 bytes per character code: the width in the low nibble of byte 0, then the column bytes
 * (0xEF05); read by lcd_render_text, lcd_text_width */
extern const u8 lcd_font[];

/* The speed bar: an (lcd_buf offset, mask) pair per bar segment (0xF450); read by lcd_set_bar */
extern const u8 lcd_bar_segments[];

/* The text dot matrix, 38 columns x 5 dots: per column five (lcd_buf offset, mask) pairs, one
 * per dot, bit 7 of the glyph's column byte first (0xF46C-0xF5E7); read by lcd_render_text */
extern const u8 lcd_text_dots[];

/* The characters of the name editor in knob order (0xF305): ui_control.c, ui_helpers.c */
#define CHARSET_INDEX_MAP_SIZE      75
extern const u8 charset_index_map[CHARSET_INDEX_MAP_SIZE];

/* {min, max} per protocol (lok_record_t.protocol), for the knob */
extern const s16 lok_address_range[][2];   /* 0xEC2F */
extern const s16 lok_range_ec43[][2];      /* 0xEC43 */
extern const s16 lok_param_range[][2];     /* 0xEC57 */

/* Per language (language_index) an array of string pointers (0xE250); the strings are in
 * the display character set */
extern const u8 *const *const lang_table[];
#define LANG(k) (lang_table[language_index][k])

/* The bootloader's flash metadata (gcc/ms1.ld) */
extern const u16 flash_sw_id;              /* 0xFF10 */
extern const u16 flash_hw_id;              /* 0xFF12 */
extern const u16 flash_vendor_id2;         /* 0xFF18 */
extern const u16 flash_app_crc_expected;   /* 0xFF1A */
extern const u16 flash_compat_flags;       /* 0xFF2C */
extern const u16 flash_adc0_scale;         /* 0xFF60 */
extern const u16 flash_adc1_scale;         /* 0xFF62 */
extern const u16 flash_adc3_scale;         /* 0xFF66 */
extern const u16 flash_adc4_scale;         /* 0xFF68: the track current sense */
extern const u16 flash_current_limit;      /* 0xFF7A: mA */

/* MM address trits per address index (0xF35A): each byte 4 trits as bit pairs, 00 "0",
 * 11 "1", 10 open; index 0 all open */
extern const u8 mm_trit_table[];

/* MM data bytes (0xF3BA): per packet kind 0-4, 15 speed steps for each of two states */
extern const u8 mm_data_table[];

/* The object ids the UI steps through (0xEC0D): the device's own (0, 1, 2), 0x100, 0x101,
 * the slots 0x200-0x209 */
extern const u16 lok_id_table[15];

/* Function icon per block position (0xEB38) */
extern const u8 rom_func_icon_table[];

#endif /* ROM_TABLES_H */
