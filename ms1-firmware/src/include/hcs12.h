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

#ifndef NULL
#define NULL ((void *)0)
#endif

/* Interrupt masking (CCR: I = 0x10, X = 0x40; software can clear X but not set it) */
#define IRQ_DISABLE()   __asm__ __volatile__ ("orcc #0x50")
#define IRQ_ENABLE()    __asm__ __volatile__ ("andcc #0xAF")

#endif /* HCS12_H */
