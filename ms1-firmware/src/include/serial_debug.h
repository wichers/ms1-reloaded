/* SCI0 debug output (hal/serial_debug.c), compiled in only with MS1_DEBUG_SERIAL: the original
 * firmware does not use SCI0, so in a normal build every call disappears with its string. */
#ifndef SERIAL_DEBUG_H
#define SERIAL_DEBUG_H

#ifdef MS1_DEBUG_SERIAL
void serial_init(void);
void serial_putc(u8 ch);
void serial_puts(const u8 *str);
void serial_puthex4(u8 nibble);
void serial_puthex8(u8 val);
void serial_puthex16(u16 val);
void serial_putdec(u16 val);
#else
#define serial_init()       ((void)0)
#define serial_putc(c)      ((void)0)
#define serial_puts(s)      ((void)0)
#define serial_puthex4(v)   ((void)0)
#define serial_puthex8(v)   ((void)0)
#define serial_puthex16(v)  ((void)0)
#define serial_putdec(v)    ((void)0)
#endif

#endif
