/*==========================================================================
 * sys/ms_tick.c - the millisecond: the modulus down counter's interrupt and the main loop's
 * pace
 *
 * The modulus down counter counts the 24 MHz bus clock down from 24000 and interrupts each
 * time it reaches 0: isr_ms_tick counts the milliseconds, the minute and the day.
 * wait_for_next_ms_tick waits until rtc_ms_counter has passed ms_tick, then counts ms_tick
 * on: one main loop pass per ms.
 *
 * Functions:
 *   isr_ms_tick            (0xDFEC) - interrupt: one millisecond
 *   timer_modulus_init     (0xE028) - start the modulus down counter
 *   timer_modulus_read     (0xE03E) - restart its count, read it
 *   wait_for_next_ms_tick  (0xD73E) - wait for the next millisecond
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/asm.h"
#include "../include/globals.h"

#define MCCTL_MCEN      0x04
#define MCCTL_FLMC      0x08        /* load the count from the modulus now */
#define MCCTL_MODMC     0x40        /* reload at 0 */
#define MCCTL_MCZI      0x80
#define TSCR1_TEN_TFFCA 0x90        /* timer on, flags cleared by accessing their register */

__asm__ (
"	.sect	.text.isr_ms_tick\n"
"	.globl	isr_ms_tick\n"
"isr_ms_tick:\n"
"	ldd	MCCNT\n"                        /* clears the flag (TFFCA) */
"	ldx	rtc_ms_counter\n"
"	inx\n"
"	stx	rtc_ms_counter\n"
"	ldx	rtc_minute_ms\n"
"	inx\n"
"	cpx	#60000\n"
"	bcs	.Lms_minute\n"
"	ldx	rtc_day_minutes\n"
"	inx\n"
"	cpx	#1440\n"
"	bcs	.Lms_day\n"
"	ldx	rtc_day_counter\n"
"	inx\n"
"	stx	rtc_day_counter\n"
"	ldx	#0\n"
".Lms_day:\n"
"	stx	rtc_day_minutes\n"
"	ldx	#0\n"
".Lms_minute:\n"
"	stx	rtc_minute_ms\n"
"	rti\n"
);

void timer_modulus_init(void)
{
    TSCR1 = TSCR1_TEN_TFFCA;
    MCCTL = MCCTL_MODMC;
    MCCTL = MCCTL_MCZI | MCCTL_MODMC | MCCTL_MCEN;     /* prescaler 1 */
    MCCNT = BUS_CYCLES_MS;
}

u16 timer_modulus_read(void)
{
    MCCTL |= MCCTL_FLMC;
    return MCCNT;
}

/*--------------------------------------------------------------------------
 * wait_for_next_ms_tick - Spin-wait until RTC ms counter changes
 * 0xD73E
 *
 * Reads ms_tick, spins until rtc_ms_counter differs,
 * then increments ms_tick. Used for ~1ms synchronization.
 *------------------------------------------------------------------------*/
void wait_for_next_ms_tick(void)
{
    u16 snapshot = ms_tick;

    /* Spin until ms counter changes */
    while (snapshot == rtc_ms_counter)
        ;

    /* Record that we consumed this tick */
    ms_tick++;
}
