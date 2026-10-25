/*==========================================================================
 * str.h - String and number formatting (lib/str.c)
 *========================================================================*/
#ifndef MS1_STRING_H
#define MS1_STRING_H

#include "hcs12.h"

u16 str_len(const u8 *s);                                      /* 0xBB60 */
s16 str_cmp(const u8 *a, const u8 *b);                         /* 0xBB75 */
u8 *str_ncpy(const u8 *src, u8 *dst, u16 max);                 /* 0xBBA5 */
u8 *str_cat(u8 *dst, const u8 *src);                           /* 0xBBD5 */
u8 *str_append_char(u8 *s, u8 c);                             /* 0xBBEE */
u8 *str_skip_ctrl(u8 *s);                                         /* 0xBC04 */
u8 *str_itoa(u8 *dst, u16 value, s8 radix, s16 width, u8 pad);    /* 0xBC14 */
u8 *str_format(u8 *out, const u8 *fmt, ...);                     /* 0xBD54 */
void charset_to_utf8(const u8 *src, u8 *dst, u16 size);           /* 0xC0CA */
void utf8_to_charset(const u8 *src, u8 *dst, u16 size);           /* 0xC15D */

#endif
