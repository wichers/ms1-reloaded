/*==========================================================================
 * ui/encoder.c - the knob: a quadrature encoder on PT6 (A) and PT7 (B), active low
 *
 * The main loop samples it twice a pass (encoder_sample). Each input is debounced by
 * shifting its level into a word masked with encoder_debounce_mask: a word of 0 is a steady
 * high level. A change of the debounced A/B state that is a quadrature step moves
 * encoder_count by encoder_step, either way: mode 0 stops it at 0 and encoder_range - 1,
 * any other wraps it in that range. ui_helpers.c turns the count into changes of a value.
 *
 * Functions:
 *   encoder_set_count  (0xBA96) - the count, its range and how it ends there
 *   encoder_sample     (0xBAB1) - one sample of the knob
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/ui.h"

#define ENC_A   0x40    /* PTT */
#define ENC_B   0x80

/* Count from value, in 0..max */
void encoder_set_count(u8 value, u8 max, u8 mode)
{
    encoder_count = value;
    encoder_range = max + 1;
    encoder_mode = mode;
}

/* The A/B state: bit 1 A, bit 2 B, each set for a steady high level (encoder_ab_state).
 * encoder_turn gives the direction of a change, by the last state times 2 plus the new one
 * halved: 1, -1, or 0 for no quadrature step. */
static const s8 encoder_turn[16] = {
    0, -1, 1, 0,
    1, 0, 0, -1,
    -1, 0, 0, 1,
    0, 1, -1, 0,
};

/* A step of the count: stopped at the ends of the range or wrapped */
static void encoder_move(s8 step)
{
    s16 v = step + encoder_count;
    s16 range = encoder_range;

    if (++encoder_clicks_lo == 0)
        ++encoder_clicks_hi;
    if (encoder_mode == 0) {
        if (v < 0)
            v = 0;
        else if (v >= range)
            v = range - 1;
    } else if (v < 0) {
        do
            v += range;
        while (v < 0);
    } else {
        do
            v -= range;
        while (v >= 0);
        v += range;
    }
    encoder_count = v;
}

void encoder_sample(void)
{
    u16 a = encoder_a_debounce << 1;
    u16 b = encoder_b_debounce << 1;
    u8 state = 0;
    s8 turn;

    if (!(PTT & ENC_A))
        a |= 1;
    a &= encoder_debounce_mask;
    encoder_a_debounce = a;
    if (!(PTT & ENC_B))
        b |= 1;
    b &= encoder_debounce_mask;
    encoder_b_debounce = b;
    if (a == 0)
        state = 2;
    if (b == 0)
        state |= 4;
    turn = encoder_turn[(u8)(encoder_ab_state << 1 | state >> 1)];
    encoder_ab_state = state;
    if (turn > 0)
        encoder_move(encoder_step);
    else if (turn < 0)
        encoder_move(-encoder_step);
}
