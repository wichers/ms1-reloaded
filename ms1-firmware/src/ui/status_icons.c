/*==========================================================================
 * ui/status_icons.c - the status icons and the speed bar (0xAC8E, 0xB042, 0xB18C)
 *
 * Run in ms_tick phase 3. Icons blink with ms_tick bits 8-9 (period 1024 ms).
 * system_status_flags: bit 0 STOP, bit 1 overload, bit 2 mfx registration running,
 * bit 7 timed delay running (show_message).
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/lokstack.h"
#include "../include/rom_tables.h"
#include "../include/lcd_icons.h"
#include "../include/lcd.h"
#include "../include/ui.h"


#define BLINK  (ms_tick & 0x300)

/* The mfx icon: lit for an mfx locomotive (protocol 1) in operating modes 1-2, blinking
 * 3/4 on while an mfx registration runs; otherwise lit only in a quarter of the blink
 * period while a registration runs. */
void lcd_mfx_icon(void)
{
    u8 st = system_status_flags;

    if (!(st & 0x80) && (u8)(operating_mode - 1) <= 1
        && lok_records[lok_current_slot].protocol == LOK_MFX) {
        if (!(st & 4) || BLINK != 0)
            icon_on(ICON_MFX);
        else
            icon_off(ICON_MFX);
    } else {
        if ((st & 4) && BLINK == 0x200)
            icon_on(ICON_MFX);
        else
            icon_off(ICON_MFX);
    }
}

void lcd_status_icons(void)
{
    u8 st;

    if (operating_mode != 0) {
        if (system_status_flags & 1)
            icon_on(ICON_STOP);
        else
            icon_off(ICON_STOP);
        if (system_status_flags & 2)
            icon_on(ICON_SHORT_CIRCUIT);
        else
            icon_off(ICON_SHORT_CIRCUIT);
        st = system_status_flags;
        if ((st & 2) || (st & 0x80)
            || ((u8)(operating_mode - 1) <= 1
                && (lok_records[lok_current_slot].flags & 1)))
            icon_on(ICON_WARNING);
        else
            icon_off(ICON_WARNING);
        lcd_mfx_icon();
    }
    if (system_status_flags & 0x80) {
        lcd_clear_segments(ICONS_BEFORE(ICON_MFX));
        return;
    }
    switch (operating_mode) {
    case 0:
        lcd_clear_segments(ICONS_BEFORE(ICON_SEG_COUNT));
        break;
    case 1:
    case 2:
        lcd_lok_icons();
        break;
    case 3:
        /* the menus; in the locomotive type choice (1/2/4) the chosen type blinks */
        st = menu_top;
        if (st == 1 && menu_item == 2 && menu_sub == 4) {
            lcd_clear_segments(ICONS_BEFORE(ICON_TRAIN_STEAM));
            if (menu_value3 == 1 && BLINK != 0)
                icon_on(ICON_TRAIN_ELECTRIC);
            else
                icon_off(ICON_TRAIN_ELECTRIC);
            if (menu_value3 == 3 && BLINK != 0)
                icon_on(ICON_TRAIN_STEAM);
            else
                icon_off(ICON_TRAIN_STEAM);
            if (menu_value3 == 2 && BLINK != 0)
                icon_on(ICON_TRAIN_DIESEL);
            else
                icon_off(ICON_TRAIN_DIESEL);
        } else if (st == 3 && menu_item != 0) {
            lcd_lok_icons();
        } else {
            lcd_clear_segments(ICONS_BEFORE(ICON_MFX));
        }
        icon_on(ICON_MENU);
        break;
    case 4:
        lcd_clear_segments(ICONS_BEFORE(ICON_MFX));
        break;
    }
}

/* The speed of the current locomotive (its state's speed: bit 7 direction, speed 0-127) as
 * 0-14 bar segments, blinking while its mfx decoder does not answer (LOKS_NO_ANSWER); in
 * the locomotive edit menu (2/3, 2/6) menu_value as 0-14 segments. Shown when the record's
 * speed_object (driving) or mfx_drive_cmd (menu 3) is nonzero. */
void lcd_speed_bar(void)
{
    const lok_state_t *d;
    u8 en, top, v;
    s16 r;

    if (system_status_flags & 0x80)
        goto off;
    switch (operating_mode) {
    case 1:
    case 2:
        en = lok_records[(s8)lok_current_slot].speed_object;
        break;
    case 3:
        top = menu_top;
        if (top == 3 && menu_item != 0) {
            en = lok_records[(s8)lok_current_slot].mfx_drive_cmd;
            break;
        }
        if (top == 2 && (menu_item == 3 || menu_item == 6) && menu_sub != 0) {
            lcd_set_bar((s8)((u16)(menu_value * 14 + 0xFF) >> 8), 2);
            return;
        }
        goto off;
    case 0:
    case 4:
        goto off;
    default:
        return;
    }
    if (en == 0)
        goto off;
    d = &lok_states[(s8)lok_current_slot];
    if ((d->flags & LOKS_NO_ANSWER) && BLINK == 0)
        goto off;
    v = d->speed;
    r = (s16)((v & 0x7F) * 14 + 127) >> 7;
    lcd_set_bar((s8)((v & 0x80) ? ~r : r), 1);
    return;
off:
    lcd_set_bar(0, 0);
}
