/*==========================================================================
 * sys/timer_init.c - Timer and ATD set-up
 *
 * Functions:
 *   timer_atd_init        (0xBA5E) - Timer and ATD0 set-up
 *
 * The encoder routines that follow it in the original (0xBA96, 0xBAB1) are in ui/encoder.c.
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"

/* ROM constant */
extern u8 flash_encoder_step;    /* 0xFF64: the knob step of this device; 0 keeps 1 */

/*--------------------------------------------------------------------------
 * timer_atd_init - Timer and ATD0 set-up (0xBA5E)
 *
 * encoder_step from the flash metadata byte 0xFF64 when that is not 0; timer on; ATD0 in
 * continuous conversion of channels 0-7; PB7 output low; pull-ups on ports A and B.
 *------------------------------------------------------------------------*/
void timer_atd_init(void)
{
    u8 flags = *(const u8 *)&flash_encoder_step;

    if (flags != 0)
        encoder_step = flags;
    TSCR1 = 0x90;           /* timer on, fast flag clear */
    TCTL3 &= 0x0F;          /* no input capture edges on channels 6, 7 */
    TIE &= 0x3F;
    ATD0CTL2 = 0xC0;        /* ATD on, fast flag clear */
    ATD0CTL3 = 0;
    ATD0CTL4 = 0x6C;
    PORTB &= 0x7F;
    DDRB |= 0x80;
    PUCR |= 0x01;           /* pull-ups on ports A and B */
    PUCR |= 0x02;
    ATD0CTL5 = 0x30;        /* continuous conversion of channels 0-7 */
}
