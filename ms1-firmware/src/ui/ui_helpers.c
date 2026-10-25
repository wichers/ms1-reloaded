/*==========================================================================
 * ui/ui_helpers.c - Speed knob and encoder value helpers, character index
 *
 * The encoder interrupt-free sampling (encoder_sample) counts quadrature
 * transitions into encoder_count (0x1101), wrapping in encoder_range; the helpers here turn
 * those counts into changes of a value.
 *
 * Functions:
 *   ui_knob_speed          (0xD7E0) - locomotive speed byte from the knob
 *   charset_index_lookup     (0xDAFF) - index of a character in the name character set
 *   ui_knob_reset   (0xDB25) - restart counting at 0
 *   ui_knob_adjust  (0xDB3C) - apply the knob to a value
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/eeprom_config.h"
#include "../include/rom_tables.h"
#include "../include/ui.h"


/* 0xD7E0: a speed byte (bit 7 direction, bits 0-6 speed) changed by the knob, in the knob
 * mode of the settings (ui_settings bits 0-1):
 *   0  speed 0-127, direction kept
 *   1  one range through zero: turning below 0 reverses (reverse speed s counts as -1-s)
 *   2, 3  speed 0-127 forward, 0-126 in reverse, direction kept */
u8 ui_knob_speed(u8 speed)
{
    s16 r;

    switch (ui_settings & 3) {
    case 1:
        r = ui_knob_adjust((speed & 0x80) ? (s16)(0xFF00 | speed) ^ 0x7F : speed, 0x7F, -0x80, 0x0F);
        return (s8)r >= 0 ? (u8)r : (u8)r ^ 0x7F;
    case 0:
        return (u8)ui_knob_adjust(speed & 0x7F, 0x7F, 0, 0x0F) | (speed & 0x80);
    default:
        if (!(speed & 0x80))
            return (u8)ui_knob_adjust(speed, 0x7F, 0, 0x0F);
        return (u8)ui_knob_adjust((s16)(0xFF00 | speed) ^ 0x7F, -1, -0x7F, 0x0F) ^ 0x7F;
    }
}

/* 0xDAFF: position of c in charset_index_map, 0 if it is not there */
u16 charset_index_lookup(u8 c)
{
    u8 n = 0;
    u16 i = 0;

    do {
        if (charset_index_map[i] == c)
            return i;
        n++;
        i++;
    } while (n <= 0x4A);
    return 0;
}

/* 0xDB25: count from 0 in a range of 256, wrapping */
void ui_knob_reset(void)
{
    encoder_set_count(0, 0xFF, 1);
    encoder_last_position = 0;
}

/* 0xDB3C: value changed by the knob since the last call, kept in min..max.
 * flags: 1 clamp to min..max (else wrap around), 2 at a value of 0 or -1 ignore up to 5
 * counts and take more as one step, 4 accelerate fast turns, 8 take the counts as steps
 * (else one step per 4 counts, the remainder kept) */
s16 ui_knob_adjust(s16 value, s16 max, s16 min, u8 flags)
{
    s8 raw = (s8)encoder_count;
    s16 steps = raw;
    s16 delta, mag;

    if (flags & 4) {
        delta = steps - (s16)encoder_last_position;
        encoder_last_position = steps;
        mag = delta < 0 ? -delta : delta;
        if (mag > 10)
            steps += delta * 31;
        else if (mag > 9)
            steps += delta * 14;
        else if (mag > 8)
            steps += delta * 9;
        else if (mag > 7)
            steps += delta * 4;
        else if (mag > 6)
            steps += delta * 3;
        else if (mag > 5)
            steps += delta * 2;
        else if (mag > 3)
            steps += delta;
    }
    if ((u16)(value + 1) <= 1 && (flags & 2)) {
        if ((u16)(steps + 5) > 10)
            steps = steps == 0 ? 0 : steps < 0 ? -1 : 1;
        else
            steps = 0;
    }
    if (flags & 8) {
        if (steps != 0)
            ui_knob_reset();
    } else {
        if (steps < 0)
            steps += 3;
        steps >>= 2;
        if (steps != 0) {
            encoder_set_count(raw & 3, 0xFF, 1);
            encoder_last_position = encoder_count & 3;
        }
    }
    value += steps;
    if (flags & 1) {
        if (min > value)
            value = min;
        if (max < value)
            value = max;
    } else {
        while (min > value)
            value += max - min + 1;
        while (max < value)
            value += min - max - 1;
    }
    return value;
}
