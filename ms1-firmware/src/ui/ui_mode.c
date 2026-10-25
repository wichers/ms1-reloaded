/*==========================================================================
 * ui/ui_mode.c - the operating mode
 *
 * Functions:
 *   ui_enter_mode  (0x9277) - switch the operating mode
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/lokstack.h"
#include "../include/ui.h"

/*--------------------------------------------------------------------------
 * ui_enter_mode - Switch the operating mode (0x9277)
 *
 * Sets operating_mode, resets the encoder value, makes lok_current_slot a slot in use
 * (or -1), and does what entering the mode needs; modes 1 and 2 fall back to mode 3 when
 * no slot is in use. Mode 0 starts the track signal ramp (master).
 *------------------------------------------------------------------------*/
void ui_enter_mode(u16 mode)
{
    for (;;) {
        operating_mode = (u8)mode;
        ui_knob_reset();
        lok_current_slot = (u8)lok_next_used_slot((s8)lok_current_slot, 0);
        switch (mode) {
        case 0:
            if (is_master != 0)
                system_status_flags |= 0x01;
            menu_top = 0x3E;
            track_fault_timer = 0;
            return;
        case 1:
            if ((s8)lok_current_slot < 0)
                break;
            if (!(lok_states[(s8)lok_current_slot].flags & LOKS_DRIVEN))
                lok_take_current();
            return;
        case 2:
            if (lok_states[(s8)lok_current_slot].flags & LOKS_DRIVEN)
                lok_release_current();
            menu_value = (s8)lok_current_slot;
            if ((s8)lok_current_slot < 0)
                break;
            return;
        case 3:
        case 4:
            menu_top = 1;
            menu_item = 0;
            return;
        default:
            return;
        }
        mode = 3;
    }
}
