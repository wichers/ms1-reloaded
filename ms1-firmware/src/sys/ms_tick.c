/*==========================================================================
 * sys/ms_tick.c - the main loop's millisecond
 *
 * wait_for_next_ms_tick (0xD73E): wait until isr_ms_tick (build/asm/hal.s) has counted
 * rtc_ms_counter past ms_tick, then count ms_tick on: one main loop pass per ms.
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"

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
