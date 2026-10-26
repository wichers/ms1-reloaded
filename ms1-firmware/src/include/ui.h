#ifndef UI_H
#define UI_H

#include "hcs12.h"

/*==========================================================================
 * The user interface: knob, keys, screens (Sources/ui/)
 *========================================================================*/

/* ui/ui_control.c */
void ui_task(void);                                                 /* 0x95A0 */

/* ui/ui_mode.c */
void ui_enter_mode(u16 mode);                                       /* 0x9277 */

/* ui/screen_text.c: the text line */
void ui_screen_text(void);                                          /* 0xB2D2 */
void lcd_draw_text(void);                                           /* 0xB9B7 */

/* ui/status_icons.c */
void lcd_mfx_icon(void);                                            /* 0xAC8E */
void lcd_status_icons(void);                                        /* 0xB042 */
void lcd_speed_bar(void);                                           /* 0xB18C */

/* ui/lok_icons.c: the locomotive's function icons */
void lcd_lok_icons(void);                                           /* 0xACF1 */
void lcd_lok_icon_segments(void);                                   /* 0xAD48 */

/* ui/ui_helpers.c: the knob */
u8   ui_knob_speed(u8 speed);                                       /* 0xD7E0 */
u16  charset_index_lookup(u8 c);                                    /* 0xDAFF */
void ui_knob_reset(void);                                           /* 0xDB25 */
s16  ui_knob_adjust(s16 value, s16 max, s16 min, u8 flags);         /* 0xDB3C */

/* ui/encoder.c: the knob's counter */
void encoder_set_count(u8 value, u8 max, u8 mode);                  /* 0xBA96 */
void encoder_sample(void);                                          /* 0xBAB1 */

/* ui/lcd_text.c: text placement and menu levels */
void lcd_scroll_step(u16 cols);                                     /* 0x9144 */
void lcd_text_layout(void);                                         /* 0x9196 */
void lcd_text_layout_left(void);                                    /* 0x91F9 */
void menu_enter(void);                                              /* 0x9239 */
void menu_back(void);                                               /* 0x925E */

#endif /* UI_H */
