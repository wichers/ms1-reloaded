/*==========================================================================
 * ui/screen_text.c - the text line of the display
 *
 * Functions:
 *   ui_screen_text  (0xB2D2) - compose the text of the current screen
 *   lcd_draw_text   (0xB9B7) - draw it
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/str.h"
#include "../include/ui.h"
#include "../include/rom_tables.h"
#include "../include/lcd.h"
#include "../include/lokstack.h"

/*--------------------------------------------------------------------------
 * ui_screen_text - The text of the current screen (0xB2D2)
 *
 * Composes lcd_text for the operating mode and the menu path (menu_top, menu_item,
 * menu_sub) and places it (lcd_text_layout), with the texts of the current language:
 *   negative system_status_flags  a message, LANG(0x16 + message_id)
 *   mode 0  "MÄRKLIN" (start-up)
 *   mode 1, 2  the selected locomotive's name; LANG(0x19) blinking in when its slot asks
 *   mode 3  the menus: new locomotive (1), change locomotive (2), delete (3), options (4)
 *   mode 4  the EEPROM record of kind menu_top, in hex
 *------------------------------------------------------------------------*/
#define LOK_ENTRY(slot) (&lok_records[(s8)(slot)])

static const u8 *const record_names[4] = { (const u8 *)0x4100, (const u8 *)"OT", (const u8 *)"OL", (const u8 *)"CL" };

void ui_screen_text(void)
{
    u8 *buf = lcd_text;
    u8 *p;
    lok_record_t *e;
    u8 top, item, sub, saved;
    u16 v, k, cols;

    if ((s8)system_status_flags < 0) {
        str_format(buf, (const u8 *)"%s", LANG(0x16 + message_id));
        lcd_text_layout();
        return;
    }
    switch (operating_mode) {
    case 0:
        str_format(buf, (const u8 *)"MaRKLIN");
        lcd_text_layout();
        return;
    case 1:
    case 2:
        utf8_to_charset(LOK_ENTRY(lok_current_slot)->name, buf, 0x10);
        if ((LOK_ENTRY(lok_current_slot)->flags & 1) && (ms_tick & 0x400))
            str_format(buf, (const u8 *)"%s", LANG(0x19));
        lcd_text_layout();
        return;
    case 3:
        break;
    case 4:
        top = menu_top;
        v = eeprom_record[top - 1][1];
        if ((u8)v == top)
            p = str_format(buf, (const u8 *)"%s ", record_names[top]);
        else
            p = str_format(buf, (const u8 *)"%x%02ux", top, (u8)v);
        str_format(p, (const u8 *)"%04ux%02ux", eeprom_record[top - 1][0], (u8)(v >> 8));
        return;
    default:
        return;
    }

    /* the menus */
    item = menu_item;
    if (item == 0) {
        str_format(buf, (const u8 *)"%s", LANG(menu_top - 1));
        lcd_text_layout();
        return;
    }
    switch (menu_top) {
    case 1:                                     /* new locomotive */
        sub = menu_sub;
        if (sub == 0) {
            str_format(buf, (const u8 *)"%s", LANG(item + 3));
        } else if (item == 2) {
            if (sub == 3)
                str_format(buf, (const u8 *)"A %3D", menu_value2);
            else if (sub == 4)
                str_format(buf, (const u8 *)"%s", LANG(0x1E));
            else
                str_format(buf, (const u8 *)"");
        } else {
            str_format(buf, (const u8 *)"");
        }
        lcd_text_layout();
        return;
    case 2:                                     /* change locomotive */
        sub = menu_sub;
        if (sub == 0) {
            str_format(buf, (const u8 *)"%s", LANG(item + 6));
            lcd_text_layout();
            return;
        }
        switch (item) {
        case 1:
            if (sub == 2) {
                p = str_format(buf, (const u8 *)"A %3D", menu_value);
                e = LOK_ENTRY(lok_current_slot);
                if (e->protocol == LOK_MM && (e->protocol_flags & LOKP_F5_F8))
                    str_format(p, (const u8 *)"/%.3D", menu_value == 0xFF ? 1 : menu_value + 1);
            } else if (sub == 3) {
                str_format(buf, (const u8 *)"%s", LANG(0x20));
            } else {
                str_format(buf, (const u8 *)"");
            }
            lcd_text_layout();
            return;
        case 2:                                 /* name editing: the cursor character inverse */
            str_format(buf, (const u8 *)"%s", lok_name_edit_buf);
            if (buf[menu_value] == ' ')
                buf[menu_value] = 0xF9;
            else
                buf[menu_value] |= 0x80;
            saved = buf[menu_value];
            buf[menu_value] = 0;
            cols = lcd_text_width(buf, (ui_settings & 0x0C) == 4, 0x7F);
            buf[menu_value] = saved;
            lcd_text_offset = (cols << 5) + 0xFDA0;
            return;
        case 3:
        case 6:
            v = menu_value * 100;
            str_format(buf, (const u8 *)"%s\x01%3d%%", LANG(item + 6), (u16)(((u32)v * 0x8081) >> 23));
            lcd_text_layout();
            return;
        case 4:
        case 5:
            k = LOK_ENTRY(lok_current_slot)->protocol == LOK_MFX ? 0x19 : 0x57;
            v = k * menu_value;
            str_format(buf, (const u8 *)"%s\x01%2d.%02d\x01S", LANG(item + 6), v / 100, v % 100);
            lcd_text_layout();
            return;
        case 7:
            if (sub == 1)
                str_format(buf, (const u8 *)"%s %2D\x02  ", LANG(0x0D), menu_value);
            else
                str_format(buf, (const u8 *)"%s %2d:%02D", LANG(0x0D), menu_value, menu_value2);
            lcd_text_layout();
            return;
        case 8:
            str_format(buf, (const u8 *)"%s?", LANG(0x0E));
            lcd_text_layout();
            return;
        default:
            return;
        }
    case 3:                                     /* delete locomotive */
        utf8_to_charset(LOK_ENTRY(lok_current_slot)->name, buf, 0x10);
        if (ms_tick & 0x400)
            str_format(buf, (const u8 *)"%s", LANG(0x1F));
        lcd_text_layout();
        return;
    case 4:                                     /* options */
        sub = menu_sub;
        if (sub == 0) {
            str_format(buf, (const u8 *)"%s", LANG(item + 0x0E));
            lcd_text_layout();
            return;
        }
        if (item == 1) {
            str_format(buf, (const u8 *)"%s", LANG(sub + 0x11));
            lcd_text_layout();
            return;
        }
        if (item == 3) {
            str_format(buf, (const u8 *)"%S", LANG(0x16));
            lcd_text_layout();
            return;
        }
        if (item != 2)
            return;
        switch (sub) {                          /* information */
        case 1:
            utf8_to_charset(device_name, buf, 0x10);
            break;
        case 2:
            str_format(buf, (const u8 *)"HW: %v %ux", flash_hw_version, flash_hw_code);
            if (porta_buttons_raw == 0xA5)
                str_format(buf, (const u8 *)"PS RS");
            break;
        case 3:
            str_format(buf, (const u8 *)"BL: %v", *(const u16 *)0xFF24);
            if (porta_buttons_raw == 0xA5)
                str_format(buf, (const u8 *)"PS");
            break;
        case 4:
            str_format(buf, (const u8 *)"AP: %v %ux", *(const u16 *)0xF704, *(const u16 *)0xF702);
            if (porta_buttons_raw == 0xA5)
                str_format(buf, (const u8 *)"PS");
            break;
        case 5:
            str_format(buf, (const u8 *)"AN: %s", *(const u8 *const *)0xF73A);
            break;
        case 6:
            str_format(buf, (const u8 *)"U%04ux%04ux", flash_uid_hi, flash_uid_lo);
            break;
        case 7:
            if (!(ms_tick & 0xE0))
                str_format(buf, (const u8 *)"UT: %2d.%dV", adc_track_voltage / 1000, adc_track_voltage % 1000 / 100);
            break;
        case 8:
            if (!(ms_tick & 0xE0))
                str_format(buf, (const u8 *)"UD: %2d.%dV", adc_decoder_voltage / 1000, adc_decoder_voltage % 1000 / 100);
            break;
        case 9:
#if MS1_RELEASE == 0x4010
            {                                   /* the over-current threshold, every pass */
                u16 a = flash_current_limit;
                str_format(buf, (const u8 *)"OVC: %d.%dA", a / 1000, a / 100 % 10);
            }
#else
            if (!(ms_tick & 0xE0)) {
                u16 a = flash_current_limit;
                str_format(buf, (const u8 *)"IT:\x02%d.%d\x1b%d.%dA", track_current / 1000, track_current % 1000 / 100,
                            a / 1000, a % 1000 / 100);
            }
#endif
            break;
        }
        lcd_text_layout();
        return;
    default:
        return;
    }
}

/*--------------------------------------------------------------------------
 * lcd_draw_text - Draw the text line (0xB9B7)
 *
 * The text in lcd_text from lcd_text_offset (in 1/32 columns, signed), blinking characters
 * in the phase of bits 8-9 of the millisecond count.
 *------------------------------------------------------------------------*/
void lcd_draw_text(void)
{
    lcd_render_text(lcd_text, ms_tick & 0x300, (s16)(lcd_text_offset << 2) >> 7);
}
