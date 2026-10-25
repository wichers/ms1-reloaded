/*==========================================================================
 * lib/str.c - String and number formatting (0xBB60-0xC23D)
 *
 * Functions:
 *   str_len           (0xBB60) - length, 0 for NULL
 *   str_cmp           (0xBB75) - compare: -1, 0, 1
 *   str_ncpy          (0xBBA5) - bounded copy, returns the end
 *   str_cat           (0xBBD5) - append a string, returns the end
 *   str_append_char  (0xBBEE) - append a character, returns the end
 *   str_skip_ctrl        (0xBC04) - step over a leading character 0x01-0x20
 *   str_itoa             (0xBC14) - number to text
 *   str_format          (0xBD54) - formatted text (format below)
 *   charset_to_utf8      (0xC0CA) - display character set to UTF-8
 *   utf8_to_charset      (0xC15D) - UTF-8 to the display character set
 *
 * The functions returning "the end" return a pointer to the terminating 0 they wrote.
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/str.h"
#include <stdarg.h>

/* Pairs {display character, Latin-1 code}, ended by a 0 display character: the characters
 * that exist in both (0xECD5, in the original's read-only data) */
extern const u8 charset_latin1_map[];

u16 str_len(const u8 *s)
{
    const u8 *p = s;

    if (s == NULL)
        return 0;
    if (*p != 0)
        while (*++p != 0)
            ;
    return (u16)(p - s);
}

s16 str_cmp(const u8 *a, const u8 *b)
{
    for (;;) {
        if (*a < *b)
            return -1;
        if (*a > *b)
            return 1;
        if (*a == 0)
            return 0;
        a++;
        b++;
    }
}

/* Copy src to dst, at most max - 1 characters and the terminator (max 0: no limit) */
u8 *str_ncpy(const u8 *src, u8 *dst, u16 max)
{
    u8 *end;

    if (dst == NULL)
        return NULL;
    if (src == NULL) {
        *dst = 0;
        return dst;
    }
    end = dst + max;
    do
        *dst = *src++;
    while (*dst++ != 0 && dst != end);
    *--dst = 0;
    return dst;
}

u8 *str_cat(u8 *dst, const u8 *src)
{
    if (dst == NULL)
        return NULL;
    if (*dst != 0)
        while (*++dst != 0)
            ;
    return str_ncpy(src, dst, 0);
}

u8 *str_append_char(u8 *s, u8 c)
{
    if (s == NULL)
        return NULL;
    if (*s != 0)
        while (*++s != 0)
            ;
    *s = c;
    *++s = 0;
    return s;
}

u8 *str_skip_ctrl(u8 *s)
{
    if (s == NULL)
        return NULL;
    if ((u8)(*s - 1) <= 0x1F)
        s++;
    return s;
}

/* value in radix 2-36 (a negative radix: value is signed, '-' before negative numbers),
 * right-aligned in width characters filled with pad (width -16..16; a negative width: exactly
 * -width characters, the lowest digits). Up to 20 characters. value and radix are
 * worked on in place, as in the original. */
u8 *str_itoa(u8 *dst, u16 value, s8 radix, s16 width, u8 pad)
{
    u8 buf[20];
    s16 n, w;
    u16 neg = 0, d;
    s16 r;

    if (width <= -16)
        w = -16;
    else if (width > 16)
        w = 16;
    else
        w = width;
    if (radix < 0) {
        radix = -radix;
        if ((s16)value < 0) {
            value = -value;
            neg = 1;
        }
    }
    r = radix > 35 ? 36 : radix < 2 ? 2 : radix;
    n = 0;
    if (value == 0) {
        buf[n++] = '0';
    } else {
        do {
            d = value % (u16)r;
            buf[n++] = (s16)d > 9 ? d + 'A' - 10 : d + '0';
            value /= (u16)r;
        } while (value != 0);
    }
    if (neg)
        buf[n++] = '-';
    while ((w < 0 ? -w : w) > n)
        buf[n++] = pad;
    if (w < 0)
        n = -w;
    if (dst == NULL)
        return NULL;
    while (n > 0)
        *dst++ = buf[--n];
    *dst = 0;
    return dst;
}

/* Formatted text into out; returns the end. A conversion is
 *   % [0] [width] [.size] [u|U] type
 * with type d (decimal), D (decimal in inverse characters, bit 7 set), x (hex), b (binary),
 * c (character), s (string), S (string in inverse characters), v (version: high byte, '.',
 * low byte); numbers are signed unless u/U is given, right-aligned in width filled with
 * '0' or blanks; the field is filled with blanks to size characters. %% is '%'. */
u8 *str_format(u8 *out, const u8 *fmt, ...)
{
    va_list ap;
    u8 *p = out, *field, *start;
    const u8 *s;
    u16 i, v;
    s16 width, size, n;
    u8 zero, uns, c;

    if (fmt == NULL)
        return out;
    va_start(ap, fmt);
    i = 0;
    *out = 0;
    while (fmt[i] != 0) {
        c = fmt[i];
        if (c != '%') {
            i++;
            p = str_append_char(p, c);
            continue;
        }
        field = p;
        i++;
        if (fmt[i] == '%') {
            i++;
            p = str_append_char(p, '%');
            continue;
        }
        width = 0;
        zero = 0;
        size = 0;
        uns = 0;
        if (fmt[i] == '0') {
            i++;
            zero = 1;
        }
        while ((u8)(fmt[i] - '0') <= 9) {
            width = width * 10 + fmt[i] - '0';
            i++;
        }
        if (fmt[i] == '.') {
            i++;
            while ((u8)(fmt[i] - '0') <= 9) {
                size = size * 10 + fmt[i] - '0';
                i++;
            }
        }
        if (fmt[i] == 'u' || fmt[i] == 'U') {
            i++;
            uns = 1;
        }
        c = fmt[i++];
        switch (c) {
        case 'D':
        case 'd':
            start = p;
            p = str_itoa(p, va_arg(ap, int), uns ? 10 : -10, width, zero ? '0' : ' ');
            if (c == 'D')
                while (start < p)
                    *start++ |= 0x80;
            break;
        case 'b':
            p = str_itoa(p, va_arg(ap, int), uns ? 2 : -2, width, zero ? '0' : ' ');
            break;
        case 'x':
            p = str_itoa(p, va_arg(ap, int), uns ? 16 : -16, width, zero ? '0' : ' ');
            break;
        case 'c':
            for (n = width - 1; n > 0; n--)
                *p++ = ' ';
            *p = (u8)va_arg(ap, int);
            *++p = 0;
            break;
        case 'S':
        case 's':
            s = va_arg(ap, const u8 *);
            for (n = width - (s16)str_len(s); n > 0; n--)
                *p++ = ' ';
            *p = 0;
            start = p;
            p = str_cat(p, s);
            if (c == 'S')
                while (start < p)
                    *start++ |= 0x80;
            break;
        case 'v':
            v = (u16)va_arg(ap, int);
            p = str_itoa(p, (s8)(v >> 8), 10, 0, ' ');
            p = str_append_char(p, '.');
            p = str_itoa(p, v & 0xFF, 10, 0, ' ');
            break;
        }
        for (n = size - (s16)(p - field); n > 0; n--)
            *p++ = ' ';
        *p = 0;
    }
    va_end(ap);
    return p;
}

/* src in the display character set to UTF-8 in dst (size bytes with the terminator):
 * characters not in charset_latin1_map are left out */
void charset_to_utf8(const u8 *src, u8 *dst, u16 size)
{
    const u8 *m;
    u8 c, code;

    if (dst == NULL)
        return;
    if (src == NULL) {
        *dst = 0;
        return;
    }
    for (c = *src; c != 0; c = *++src) {
        for (m = charset_latin1_map; m[0] != 0 && m[0] != c; m += 2)
            ;
        if (m[0] == 0)
            continue;
        code = m[1];
        if (code & 0x80) {
            if (size <= 2)
                break;
            *dst++ = 0xC0 + (code >> 6);
            *dst++ = 0x80 + (m[1] & 0x3F);
            size -= 2;
        } else {
            if (--size == 0)
                break;
            *dst++ = code;
        }
    }
    *dst = 0;
}

/* UTF-8 src to the display character set in dst (size characters and the terminator):
 * characters not in charset_latin1_map become 0x7F, stray continuation bytes are left out */
void utf8_to_charset(const u8 *src, u8 *dst, u16 size)
{
    const u8 *m;
    u16 code;
    u8 c;

    if (dst == NULL)
        return;
    if (src == NULL) {
        *dst = 0;
        return;
    }
    c = *src;
    while (c != 0 && size != 0) {
        if (!(c & 0x80)) {
            code = c;
            src++;
        } else if (!(c & 0x40)) {
            code = 0xFFFF;              /* continuation byte: nothing */
            src++;
        } else if (c & 0x20) {
            code = 0xFFFE;              /* sequence of 3 or more bytes */
            src++;
        } else {
            code = (u16)(c & 0x1F) << 6;
            c = *++src;
            code |= c & 0x3F;
            if (c != 0)
                src++;
        }
        if (code != 0xFFFF) {
            for (m = charset_latin1_map + 1; m[0] != 0 && m[0] != code; m += 2)
                ;
            if (m[0] == 0)
                code = 0xFFFE;
            if (code == 0xFFFE) {
                *dst++ = 0x7F;
                size--;
            } else {
                *dst++ = m[-1];
                size--;
            }
        }
        c = *src;
    }
    *dst = 0;
}
