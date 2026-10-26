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
 * that exist in both (the comments show the Latin-1 ones) */
static const u8 charset_latin1_map[] = {
    0x20, 0x20, 0x21, 0x21, 0x22, 0x22, 0x23, 0x23, 0x24, 0x24, 0x25, 0x25, 0x26, 0x26, 0x27, 0x27,   /*  !"#$%&' */
    0x28, 0x28, 0x29, 0x29, 0x2A, 0x2A, 0x2B, 0x2B, 0x2C, 0x2C, 0x2D, 0x2D, 0x2E, 0x2E, 0x2F, 0x2F,   /* ()*+,-./ */
    0x30, 0x30, 0x31, 0x31, 0x32, 0x32, 0x33, 0x33, 0x34, 0x34, 0x35, 0x35, 0x36, 0x36, 0x37, 0x37,   /* 01234567 */
    0x38, 0x38, 0x39, 0x39, 0x3A, 0x3A, 0x3B, 0x3B, 0x3C, 0x3C, 0x3D, 0x3D, 0x3E, 0x3E, 0x3F, 0x3F,   /* 89:;<=>? */
    0x40, 0x40, 0x41, 0x41, 0x42, 0x42, 0x43, 0x43, 0x44, 0x44, 0x45, 0x45, 0x46, 0x46, 0x47, 0x47,   /* @ABCDEFG */
    0x48, 0x48, 0x49, 0x49, 0x4A, 0x4A, 0x4B, 0x4B, 0x4C, 0x4C, 0x4D, 0x4D, 0x4E, 0x4E, 0x4F, 0x4F,   /* HIJKLMNO */
    0x50, 0x50, 0x51, 0x51, 0x52, 0x52, 0x53, 0x53, 0x54, 0x54, 0x55, 0x55, 0x56, 0x56, 0x57, 0x57,   /* PQRSTUVW */
    0x58, 0x58, 0x59, 0x59, 0x5A, 0x5A, 0x5B, 0x5B, 0x5C, 0x5C, 0x5D, 0x5D, 0x5E, 0x5E, 0x5F, 0x5F,   /* XYZ[\]^_ */
    0x60, 0x60, 0x41, 0x61, 0x42, 0x62, 0x43, 0x63, 0x44, 0x64, 0x45, 0x65, 0x46, 0x66, 0x47, 0x67,   /* `abcdefg */
    0x48, 0x68, 0x49, 0x69, 0x4A, 0x6A, 0x4B, 0x6B, 0x4C, 0x6C, 0x4D, 0x6D, 0x4E, 0x6E, 0x4F, 0x6F,   /* hijklmno */
    0x50, 0x70, 0x51, 0x71, 0x52, 0x72, 0x53, 0x73, 0x54, 0x74, 0x55, 0x75, 0x56, 0x76, 0x57, 0x77,   /* pqrstuvw */
    0x58, 0x78, 0x59, 0x79, 0x5A, 0x7A, 0x7B, 0x7B, 0x7C, 0x7C, 0x7D, 0x7D, 0x7E, 0x7E, 0x62, 0xAB,   /* xyz{|}~« */
    0x63, 0xBB, 0x41, 0xC0, 0x41, 0xC1, 0x41, 0xC2, 0x41, 0xC3, 0x61, 0xC4, 0x41, 0xC5, 0x43, 0xC7,   /* »ÀÁÂÃÄÅÇ */
    0x45, 0xC8, 0x45, 0xC9, 0x45, 0xCA, 0x45, 0xCB, 0x49, 0xCC, 0x49, 0xCD, 0x49, 0xCE, 0x49, 0xCF,   /* ÈÉÊËÌÍÎÏ */
    0x4E, 0xD1, 0x4F, 0xD2, 0x4F, 0xD3, 0x4F, 0xD4, 0x4F, 0xD5, 0x6F, 0xD6, 0x65, 0xD8, 0x55, 0xD9,   /* ÑÒÓÔÕÖØÙ */
    0x55, 0xDA, 0x55, 0xDB, 0x75, 0xDC, 0x59, 0xDD, 0x64, 0xDF, 0x41, 0xE0, 0x41, 0xE1, 0x41, 0xE2,   /* ÚÛÜÝßàáâ */
    0x41, 0xE3, 0x61, 0xE4, 0x41, 0xE5, 0x43, 0xE7, 0x45, 0xE8, 0x45, 0xE9, 0x45, 0xEA, 0x45, 0xEB,   /* ãäåçèéêë */
    0x49, 0xEC, 0x49, 0xED, 0x49, 0xEE, 0x49, 0xEF, 0x4E, 0xF1, 0x4F, 0xF2, 0x4F, 0xF3, 0x4F, 0xF4,   /* ìíîïñòóô */
    0x4F, 0xF5, 0x6F, 0xF6, 0x65, 0xF8, 0x55, 0xF9, 0x55, 0xFA, 0x55, 0xFB, 0x75, 0xFC, 0x59, 0xFF,   /* õöøùúûüÿ */
    0x00, 0x00,   /* end */
};

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
