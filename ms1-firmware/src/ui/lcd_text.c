/*==========================================================================
 * ui/lcd_text.c - Placing text on the display, menu levels (0x9144-0x9276)
 *
 * The text the display shows is in lcd_text; lcd_text_offset says where it
 * starts: (columns - 38) * 16 for text that fits (centred, a negative offset), or, for
 * text wider than the 38 columns, a scroll state - bit 15 scrolling, bit 14 scrolling
 * back, below that the position in 1/32 columns.
 *
 * Functions:
 *   lcd_scroll_step        (0x9144) - one step of back-and-forth scrolling
 *   lcd_text_layout        (0x9196) - centre the text, or scroll / left-align it
 *   lcd_text_layout_left   (0x91F9) - left-align the text, or scroll it
 *   menu_enter                 (0x9239) - one menu level deeper
 *   menu_back                  (0x925E) - one menu level back
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/ui.h"
#include "../include/lcd.h"

void lcd_scroll_step(u16 cols)
{
    u16 s = lcd_text_offset;
    s16 end;

    if (!(s & 0x8000)) {
        lcd_text_offset = 0x8000;
        return;
    }
    if (s & 0x4000) {
        s -= 3;
        lcd_text_offset = s;
        if (s & 0x2000)
            lcd_text_offset = 0x8000;
        return;
    }
    s += 3;
    lcd_text_offset = s;
    end = (s16)((cols - 38) << 5);
    if ((s16)(s & 0x3FFF) < end)
        return;
    lcd_text_offset = (u16)(end - 1) | 0xC000;
}

/* Text that fits is centred; wider text scrolls when the settings ask for it (display
 * mode 8 in ui_settings bits 2-3), else it starts at the left */
void lcd_text_layout(void)
{
    u16 cols = lcd_text_width(lcd_text, (ui_settings & 0x0C) == 4, 0x27);
    u16 off = cols * 16 + 0xFDA0;

    if ((ui_settings & 0x0C) == 8) {
        if ((s16)cols > 0x27)
            lcd_scroll_step(cols);
        else
            lcd_text_offset = off & 0x7FFF;
    } else {
        lcd_text_offset = off & 0x7FFF;
        if (!(off & 0x2000))
            lcd_text_offset = 0;
    }
}

void lcd_text_layout_left(void)
{
    u16 cols = lcd_text_width(lcd_text, (ui_settings & 0x0C) == 4, 0x27);

    if ((ui_settings & 0x0C) == 8 && (s16)cols > 0x27)
        lcd_scroll_step(cols);
    else
        lcd_text_offset = 0;
}

/* The menu path is menu_top (the menu), menu_item, menu_sub, menu_sub2; 0 = not entered */
void menu_enter(void)
{
    ui_knob_reset();
    if (menu_item == 0) {
        menu_item = 1;
        menu_sub = 0;
    } else if (menu_sub == 0) {
        menu_sub = 1;
        menu_sub2 = 0;
    } else {
        menu_sub2 = 1;
    }
}

void menu_back(void)
{
    ui_knob_reset();
    if (menu_sub == 0)
        menu_item = 0;
    else if (menu_sub2 == 0)
        menu_sub = 0;
    else
        menu_sub2 = 0;
}
