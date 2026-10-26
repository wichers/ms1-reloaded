/*==========================================================================
 * sys/delay.c - busy waits, timed for the 24 MHz bus
 *
 * Functions:
 *   delay_long   (0xE072) - s seconds (nothing calls it)
 *   delay_short  (0xE084) - ms milliseconds
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/asm.h"
#include "../include/delay.h"

void delay_long(u16 s)
{
    while (s--)
        delay_short(1000);
}

/* A pass takes BUS_CYCLES_MS: the inner loop's DBNE 3 cycles each, LDX 2, NOP 1, the outer
 * DBNE 3 */
void delay_short(u16 ms)
{
    __asm__ __volatile__ (
        "	tbeq	d,2f\n"
        "1:	ldx	#(" ASM_NUM(BUS_CYCLES_MS) "-6)/3\n"
        "	nop\n"
        "3:	dbne	x,3b\n"
        "	dbne	d,1b\n"
        "2:"
        : "+d" (ms) : : "x");
}
