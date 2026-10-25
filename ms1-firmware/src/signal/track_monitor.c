/*==========================================================================
 * signal/track_monitor.c - track current monitor and overload shutdown (0x9333)
 *
 * Runs once per main loop pass (1 ms). It measures the track current, integrates the
 * current above the limit into two overload accumulators, and switches the track output
 * (timer channel 3 output compare, TIOS bit 3) off on overload or when the track voltage
 * leaves its range. It also counts operating minutes and overload events in the EEPROM
 * records (counter_inc_24bit).
 *
 * system_status_flags bits used here:
 *   0x01  stopped after too many overload shutdowns (shutdown_count > 4)
 *   0x02  overload
 *   0x40  output off for an overload shutdown, until shutdown_ticks reaches 200
 * Each change of bits 0x01/0x02 also sets the bit in mcan_status_pending.
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/rom_tables.h"

extern void counter_inc_24bit(u8 *counter);

void track_current_monitor(void)
{
    u8 status, fault;
    u16 current, v, a, t;
    s16 d;

    if (--minute_countdown == 0) {
        counter_inc_24bit(operating_minutes);
        minute_countdown = 60000;
    }
    if (operating_mode == 0) {
        track_current = (u16)(((u32)ATD0DR4 * flash_adc4_scale) >> 16);
        return;
    }
    if (is_master == 0)
        return;

    status = system_status_flags;
    fault = status & 3;
    if (fault == 2 && !(track_fault_prev & 2)) {
        counter_inc_24bit(overload_events);
        status = system_status_flags;
    }
    track_fault_prev = fault;

    /* the current: ADC channel 4 less the zero offset, at least the channel 3 value */
    current = (u16)(((u32)ATD0DR4 * flash_adc4_scale) >> 16) - track_current_offset;
    track_current = current;
    if ((s16)current < 0)
        track_current = 0;
    v = (u16)(((u32)(u16)(ATD0DR3 * 10) * flash_adc3_scale) >> 16);
    current = track_current;
    if (v > current) {
        current = v;
        track_current = v;
    }

    /* slow accumulator: 1/100 of the current above limit + 200 mA */
    d = (s16)(flash_current_limit - current + 200) / 100;
    a = overload_slow;
    if (d > 0 && a < (u16)d)
        a = 0;
    else
        a -= d;
    if (a > 10000)
        a = 10000;
    overload_slow = a;

    /* fast accumulator: the current above limit + 1200 mA */
    d = (s16)(flash_current_limit - current + 1200);
    t = overload_fast;
    if (d > 0 && t < (u16)d)
        t = 0;
    else
        t -= d;
    if (t > 10000)
        t = 10000;
    overload_fast = t;

    v = adc_track_voltage;
    if (a == 0 && overload_fast == 0 && v > 11999 && v <= 25000) {
        if (track_fault_timer != 0)
            track_fault_timer--;
        if (((status & 1) || track_fault_timer == 0) && (status & 2)) {
            status &= ~2;
            system_status_flags = status;
            mcan_status_pending |= 2;
        }
    } else {
        if (!(status & 2)) {
            status |= 2;
            system_status_flags = status;
            mcan_status_pending |= 2;
        }
        t = track_fault_timer + 3;
        track_fault_timer = t;
        if ((u16)(v - 10000) > 17000 || a > 9999 || overload_fast > 9999) {
            t = 4500;
            track_fault_timer = 4500;
        }
        if (t > 4499) {
            track_fault_timer = 4500;
            if (!(status & 0x40)) {
                shutdown_ticks = 0;
                status |= 0x40;
                system_status_flags = status;
                if (++shutdown_count > 4)
                    shutdown_count = 5;
            }
        }
    }

    t = ++shutdown_ticks;
    if (t == 200) {
        status &= ~0x40;
        system_status_flags = status;
    }
    if (t == 1000)
        shutdown_count = 0;
    if (shutdown_count > 4 && !(status & 1)) {
        status |= 1;
        system_status_flags = status;
        mcan_status_pending |= 1;
    }
    if (status & 0x41)
        TIOS &= ~0x08;
    else
        TIOS |= 0x08;
}
