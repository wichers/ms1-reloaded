/*==========================================================================
 * hal/gpio_buttons.c - the keys
 *
 * button_scan (0xD76F), every 32 ms: the keys on PORTA and PORTB (active low), each port's
 * state and the bits that changed since the last scan; how long MENU/ESC (PB0) and STOP (PB3)
 * are held.
 *
 * Functions:
 *   gpio_read_porta_active_low  (0xDFDC) - the keys of PORTA pressed, among mask
 *   gpio_read_portb_active_low  (0xDFE3) - the keys of PB0-PB4 pressed, among mask
 *   button_scan                 (0xD76F) - the keys' state and changes
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"

u16 gpio_read_porta_active_low(u8 mask)
{
    return (u8)(mask & ~PORTA);
}

u16 gpio_read_portb_active_low(u8 mask)
{
    return mask & ~PORTB & 0x1F;
}

void button_scan(void)
{
    u8 prev_porta, new_porta, porta_changed;
    u8 prev_portb, new_portb, portb_changed;

    prev_porta = porta_buttons_raw;
    porta_buttons_edge = prev_porta;

    new_porta = (u8)gpio_read_porta_active_low(0xFF);
    porta_buttons_raw = new_porta;

    porta_changed = prev_porta ^ new_porta;
    porta_buttons_edge = porta_changed;

    prev_portb = button_state_raw;
    button_state_edge = prev_portb;

    new_portb = (u8)gpio_read_portb_active_low(0xFF);
    button_state_raw = new_portb;

    portb_changed = prev_portb ^ new_portb;
    button_state_edge = portb_changed;

    /* MENU/ESC (PB0) held */
    if (new_portb & 0x01) {
        ui_esc_held++;
        if (ui_esc_held > 0x7D) ui_esc_held = 0x7D;
    } else {
        ui_esc_held = 0;
    }

    /* STOP (PB3) held */
    if (button_state_raw & 0x08) {
        ui_stop_held++;
        if (ui_stop_held > 0x7D) ui_stop_held = 0x7D;
    } else {
        ui_stop_held = 0;
    }
}
