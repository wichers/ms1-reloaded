/*==========================================================================
 * hcs12.h - the base every source includes: integer types (GCC -mshort: int is 16 bits),
 * the MC9S12D64 registers, interrupt masking
 *========================================================================*/
#ifndef HCS12_H
#define HCS12_H

#include "mc9s12d64.h"

typedef unsigned char   u8;
typedef unsigned int    u16;
typedef unsigned long   u32;
typedef signed char     s8;
typedef signed int      s16;
typedef signed long     s32;

/* The application version built (README: Release images): 0x4011 (releases 4011, c011; the
 * default) or 0x4010 (releases 4010, c010: no track current measurement, an over-current
 * input on PB6, the booster line on PB7). The Makefile sets it from RELEASE. */
#ifndef MS1_RELEASE
#define MS1_RELEASE 0x4011
#endif

/* Switch the booster off before a restart: 4011 takes timer channel 3 (PT3) off output
 * compare, 4010 drives PB7 low */
#if MS1_RELEASE == 0x4010
#define TRACK_OUTPUT_OFF()  (PORTB &= ~0x80)
#else
#define TRACK_OUTPUT_OFF()  (TIOS &= ~0x08)
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

/* Interrupt masking (CCR: I = 0x10, X = 0x40; software can clear X but not set it) */
#define IRQ_DISABLE()   __asm__ __volatile__ ("orcc #0x50")
#define IRQ_ENABLE()    __asm__ __volatile__ ("andcc #0xAF")

#endif /* HCS12_H */
