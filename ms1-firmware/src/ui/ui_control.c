/*==========================================================================
 * ui/ui_control.c - The controls: knob, keys, menus (0x95A0)
 *
 * ui_task runs every 32 ms from the main loop (slot 3, sub-state 8).
 * It handles the STOP key and the message timeouts, then the controls of the operating
 * mode (operating_mode, set by ui_enter_mode):
 *   0  start-up screen: about 2 s, measuring the track signal period; STOP and knob held:
 *      factory reset
 *   1  driving the selected locomotive: knob = speed, knob push = stop / reverse, LIGHT and
 *      the function keys, MENU = the menus, OK = choose a locomotive
 *   2  choosing a locomotive with the knob
 *   3  the menus (menu_top, menu_item, menu_sub; see ui_screen_text)
 *   4  the EEPROM records view
 *
 * Keys (button_state_raw/edge, from port B): 0x01 MENU/ESC, 0x02 OK, 0x04 LIGHT, 0x08 STOP,
 * 0x10 knob push; the function keys are port A (porta_buttons_raw/edge).
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/str.h"
#include "../include/eeprom_config.h"
#include "../include/rom_tables.h"
#include "../include/ui.h"
#include "../include/lokstack.h"
#include "../include/delay.h"

/* {min, max} per protocol (lok_record_t.protocol), for the knob: the address, the CV number
 * and the CV value of a locomotive */
static const s16 lok_address_range[5][2] = {
    { 0, 0 },   /* 0 */
    { 0, 0 },   /* LOK_MFX */
    { 1, 9999 },   /* LOK_DCC */
    { 1, 80 },   /* LOK_MM */
    { 1, 104 },   /* 4 */
};
static const s16 lok_cv_range[5][2] = {
    { 0, 0 },   /* 0 */
    { 0, 0 },   /* LOK_MFX */
    { 1, 1023 },   /* LOK_DCC */
    { 1, 79 },   /* LOK_MM */
    { 0, 0 },   /* 4 */
};
static const s16 lok_cv_value_range[5][2] = {
    { 0, 0 },   /* 0 */
    { 0, 0 },   /* LOK_MFX */
    { 0, 255 },   /* LOK_DCC */
    { 0, 80 },   /* LOK_MM */
    { 0, 0 },   /* 4 */
};

extern void show_message(u8 message, u8 ticks);       /* 0xDCA9: show a message */
extern u16  lokdb_count(void);             /* database: number of records */
extern void lokdb_read(u16 index, u8 *dest); /* database: copy record index */

/* keys pressed now (pressed and newly pressed) */
#define KEY(mask)        (button_state_edge & button_state_raw & (mask))
#define KEY_ESC          0x01
#define KEY_OK           0x02
#define KEY_LIGHT        0x04
#define KEY_STOP         0x08
#define KEY_KNOB         0x10

#define ENTRY(slot)      (&lok_records[(s8)(slot)])
#define DISP(slot)       (&lok_states[(s8)(slot)])

/* stop the track signal and restart, after erasing the EEPROM sector at addr */
static void erase_and_restart(u16 addr)
{
    IRQ_DISABLE();
    TIE &= ~0x10;
    TRACK_OUTPUT_OFF();
    eeprom_erase_sector(addr);
    delay_short(0x20);
    crt_restart();
}

/* the menu items of "change locomotive" (menu_top 2) a locomotive offers */
static u8 change_item_offered(u8 item, u8 slot)
{
    lok_record_t *e = ENTRY(slot);
    u8 proto = e->protocol;

    switch (item) {
    case 1:
        return e->kinds[0] != 0x82 && proto != LOK_MFX;
    case 2:
        return 1;
    case 3:
    case 4:
    case 5:
        if (e->kinds[0] != 0x80)
            return 0;
        if (proto == LOK_MFX && (item == 3 ? e->mfx_vmax_cv : e->mfx_accel_cv) != 0)
            return 1;
        return proto == LOK_DCC || (proto == LOK_MM && (LO8(e->protocol_flags) & LOKP_PROGRAMMABLE));
    case 6:
        if (e->kinds[0] == 0x82)
            return 0;
        if ((u8)(proto - 1) <= 1)                   /* mfx, DCC */
            return (e->protocol_flags & LOKP_VOLUME) != 0;
        return proto == LOK_MM && (LO8(e->protocol_flags) & LOKP_PROGRAMMABLE) && (e->protocol_flags & LOKP_VOLUME);
    case 7:
        if (e->kinds[0] == 0x82)
            return 0;
        return proto == LOK_DCC || (proto == LOK_MM && (LO8(e->protocol_flags) & LOKP_PROGRAMMABLE));
    case 8:
        if (e->kinds[0] == 0x82)
            return 0;
        return (u8)(proto - 1) <= 1 || (proto == LOK_MM && (LO8(e->protocol_flags) & LOKP_PROGRAMMABLE));
    default:
        return 0;
    }
}

/* mode 1: speed, direction and functions of the selected locomotive */
static void drive(void)
{
    s8 slot = (s8)lok_current_slot;
    lok_state_t *disp = DISP(slot);
    u8 speed = ui_knob_speed(disp->speed);
    u8 old, k, fn0, fn1, key;
    u16 mask;

    if (disp->controller == mcan_sd_handle && !(ENTRY(slot)->flags & 1)) {
        if (KEY(KEY_KNOB)) {
            if (ui_settings & 0x10)
                speed = (speed & 0x7F) == 0 ? speed ^ 0x80 : speed & 0x80;
            else
                speed = (speed ^ 0x80) & 0x80;
        }
        slot = (s8)lok_current_slot;
        disp = DISP(slot);
        old = disp->speed;
        if (speed != old && ENTRY(slot)->speed_object != 0) {
            if ((old ^ speed) & 0x80) {
                disp->track_pending ^= 2;
                disp->can_pending ^= 2;
            }
            disp->speed = speed;
            LO8(disp->track_pending) |= 1;
            LO8(disp->can_pending) |= 1;
        }
        /* functions: a key (port A bit, 8 = LIGHT) switches its function, a momentary one
         * (icon bit 7) only while held */
        for (k = 0; k < 16; k++) {
            lok_function_t *f = &ENTRY(slot)->functions[k];
            fn0 = f->icon;
            fn1 = f->key;
            if (fn0 == 0)
                continue;
            mask = (u16)1 << k;
            key = fn1 >> 4;
            if (key == 8) {
                if (!(button_state_edge & KEY_LIGHT))
                    continue;
                if ((s8)fn0 >= 0) {
                    if (!(button_state_raw & KEY_LIGHT))
                        continue;
                    disp->track_functions |= mask;
                    disp->can_functions |= mask;
                    disp->functions ^= mask;
                } else {
                    disp->track_functions |= mask;
                    disp->can_functions |= mask;
                    if (button_state_raw & KEY_LIGHT)
                        disp->functions |= mask;
                    else
                        disp->functions &= ~mask;
                }
                continue;
            }
            if (!((porta_buttons_edge >> key) & 1))
                continue;
            if ((s8)fn0 >= 0) {
                if (!((porta_buttons_raw >> key) & 1))
                    continue;
                disp->track_functions |= mask;
                disp->can_functions |= mask;
                disp->functions ^= mask;
            } else {
                disp->track_functions |= mask;
                disp->can_functions |= mask;
                if ((porta_buttons_raw >> key) & 1)
                    disp->functions |= mask;
                else
                    disp->functions &= ~mask;
            }
        }
    }
    if (KEY(KEY_ESC))
        ui_enter_mode(3);
    else if (KEY(KEY_OK))
        ui_enter_mode(2);
}

/* menu_top 1, menu_item 1: a new locomotive from the database (page $3C) */
static void new_from_database(u8 sub)
{
    u8 b, n, i, v;
    u8 *p, *q;
    lok_function_t *fn;
    u16 addr;

    switch (sub) {
    case 1:
        menu_value = 0;
        menu_value2 = lokdb_count();
        if (menu_value2 == 0) {
            menu_back();
            return;
        }
        menu_sub++;
        /* fall through */
    case 2:
        menu_value = ui_knob_adjust(menu_value, menu_value2 - 1, 0, 4);
        lokdb_read(menu_value, lok_name_edit_buf);
        menu_value4 = *(u16 *)lok_name_edit_buf;
        b = lokdb_rec_info;
        menu_value3 = (s8)b < 0 ? 1 : 0;
        menu_db_brand = (b & 0x40) ? 'T' : 'M';
        if (KEY(KEY_OK | KEY_KNOB)) {
            menu_sub++;
            return;
        }
        if (KEY(KEY_ESC))
            menu_back();
        return;
    case 3:
        if (lok_edit.unused != 0xFF) {
            if (lok_edit.unused == 0) {
                show_message(2, 0x3E);
                ui_enter_mode(1);
            }
            return;
        }
        b = lokdb_rec_info;
        lok_edit.symbol = (b >> 4) & 3;
        lok_edit.kinds[0] = 0x80;
        lok_edit.kinds[1] = 0x82;
        lok_edit.kinds[2] = 0;
        lok_edit.protocol = (lokdb_rec_protocol & 3) + 2;
        addr = (u8)(lokdb_rec_protocol >> 2);
        lok_edit.protocol_flags = addr;
        n = b & 0x0F;
        v = lok_name_edit_buf[5 + n];
        if ((s8)v < 0) {
            lok_edit.protocol_flags = addr | 0x1000;
            lok_name_edit_buf[5 + n] = v & 0x7F;
            b = lokdb_rec_info;
        }
        lok_edit.address = lokdb_rec_address & 0x7F;
        lok_edit.default_address = lokdb_rec_address & 0x7F;
        LO8(lok_edit.mfx_uid_lo) = 0;
        HI8(lok_edit.mfx_uid_lo) = 0;
        LO8(lok_edit.mfx_uid_hi) = 0;
        HI8(lok_edit.mfx_uid_hi) = 0;
        lok_edit.speed_object = 1;
        if ((s8)lok_name_edit_buf[6 + (b & 0x0F)] < 0) {
            lok_name_edit_buf[5 + (b & 0x0F)] &= 0x7F;
            lok_edit.speed_object = 0;
            b = lokdb_rec_info;
        }
        charset_to_utf8(&lok_name_edit_buf[5 + (b & 0x0F)], lok_edit.name, 0x10);
        n = lokdb_rec_info & 0x0F;
        fn = lok_edit.functions;
        q = &lok_name_edit_buf[5];
        for (i = 0; i < 16; i++, fn++, q++)
            fn->icon = i < n ? *q : 0;
        lok_assign_functions(10);
        lok_edit.vmax = 0xFF;
        lok_edit.accel = 4;
        lok_edit.decel = 3;
        lok_edit.volume = 0xFF;
        lok_edit.flags = 0;
        lok_edit.unused = 0;
        sub = ++menu_sub;
        /* fall through */
    case 4:
        if ((s8)lokdb_rec_address >= 0) {
            ui_enter_mode(1);
            return;
        }
        /* the record continues with another locomotive: move it to the front */
        p = &lok_name_edit_buf[5 + (lokdb_rec_info & 0x0F)];
        q = lok_name_edit_buf;
        while (*p++ != 0)
            ;
        while (p < lok_name_edit_buf + sizeof lok_name_edit_buf)
            *q++ = *p++;
        menu_sub = sub - 1;
        return;
    default:
        return;
    }
}

/* menu_top 1, menu_item 2: a new locomotive by address */
static void new_by_address(void)
{
    u8 sub = menu_sub;
    lok_function_t *f;

    if ((u8)(sub - 2) <= 2) {
        if (KEY(KEY_ESC))
            menu_back();
        else if (KEY(KEY_OK | KEY_KNOB))
            menu_sub++;
        sub = menu_sub;
    }
    switch (sub) {
    case 1:
        menu_value = 3;
        menu_value2 = 1;
        menu_value4 = 3;
        menu_value3 = 3;
        menu_sub = 3;
        /* fall through */
    case 3:
        menu_value2 = ui_knob_adjust(menu_value2, lok_address_range[menu_value][1],
                                                  lok_address_range[menu_value][0], 4);
        return;
    case 4:
        menu_value3 = ui_knob_adjust(menu_value3, 3, 0, 0);
        return;
    case 5:
        if (lok_edit.unused != 0xFF) {
            show_message(2, 0x3E);
            ui_enter_mode(1);
            return;
        }
        str_format(lok_edit.name, (const u8 *)"%d", menu_value2);
        lok_edit.symbol = (u8)menu_value3;
        lok_edit.kinds[0] = 0x80;
        lok_edit.kinds[1] = 0x82;
        lok_edit.kinds[2] = 0;
        lok_edit.protocol = (u8)menu_value;
        lok_edit.protocol_flags = menu_value4;
        lok_edit.address = menu_value2;
        lok_edit.default_address = 3;
        LO8(lok_edit.mfx_uid_lo) = 0;
        HI8(lok_edit.mfx_uid_lo) = 0;
        LO8(lok_edit.mfx_uid_hi) = 0;
        HI8(lok_edit.mfx_uid_hi) = 0;
        lok_edit.speed_object = 1;
        for (f = lok_edit.functions; f <= &lok_edit.functions[15]; f++)
            f->icon = f <= &lok_edit.functions[8] ? 0x10 : 0;
        lok_assign_functions(10);
        lok_edit.vmax = 0xFF;
        lok_edit.accel = 4;
        lok_edit.decel = 3;
        lok_edit.volume = 0xFF;
        lok_edit.flags = 0;
        lok_edit.unused = 0;
        ui_enter_mode(1);
        return;
    default:
        return;
    }
}

/* menu_top 2 (change locomotive), a menu_item entered: the value it edits, the OK that
 * stores it in the slot and marks its state for sending */
static void change_item(u8 slot, u8 sub)
{
    lok_record_t *e = ENTRY(slot);
    lok_state_t *disp = DISP(slot);
    u8 *p;
    s16 y;
    static const u8 param_bit[4] = { 0x10, 0x20, 0x40, 0x80 };
    u8 i;

    switch (menu_item) {
    case 1:                                     /* address */
        if (disp->speed & 0x7F) {
            menu_back();
            show_message(4, 0x3E);
            return;
        }
        switch (sub) {
        case 1:
            menu_value = e->address;
            menu_value2 = 0;
            menu_sub = sub + 1;
            /* fall through */
        case 2:
            menu_value = ui_knob_adjust(menu_value, lok_address_range[e->protocol][1],
                                                 lok_address_range[e->protocol][0], 4);
            if (KEY(KEY_OK | KEY_KNOB)) {
                e = ENTRY(lok_current_slot);
                if (e->protocol == LOK_DCC || (e->protocol == LOK_MM && (e->protocol_flags & LOKP_PROGRAMMABLE)))
                    menu_sub = 3;
                else
                    menu_sub = 4;
                return;
            }
            if (KEY(KEY_ESC))
                menu_back();
            return;
        case 3:
            if (!KEY(KEY_ESC | KEY_OK | KEY_KNOB))
                return;
            menu_sub = sub + 1;
            if (!((button_state_edge & button_state_raw) & 1))
                menu_value2 = 1;
            return;
        case 4:
            e->address = menu_value;
            if (menu_value2 != 0)
                LO8(disp->track_pending) |= 4;
            LO8(disp->can_pending) |= 4;
            LO8(disp->save_pending) |= 4;
            ui_enter_mode(1);
            return;
        default:
            return;
        }
    case 2:                                     /* name */
        p = &lok_name_edit_buf[menu_value];
        *p = charset_index_map[ui_knob_adjust((u8)charset_index_lookup(*p), 0x4A, 0, 4)];
        if (KEY(KEY_KNOB) && menu_value <= 13) {
            menu_value++;
            p = &lok_name_edit_buf[menu_value];
            if (*p == 0) {
                p[0] = 'A';
                p[1] = 0;
            }
        }
        if (KEY(KEY_OK)) {
            while (lok_name_edit_buf[0] == ' ')
                for (p = lok_name_edit_buf; *p != 0; p++)
                    p[0] = p[1];
            for (y = (s16)str_len(lok_name_edit_buf) - 1; y >= 0 && lok_name_edit_buf[y] == ' '; y--)
                lok_name_edit_buf[y] = 0;
            charset_to_utf8(lok_name_edit_buf, ENTRY(lok_current_slot)->name, 0x10);
            disp = DISP(lok_current_slot);
            LO8(disp->track_pending) |= 8;
            LO8(disp->can_pending) |= 8;
            LO8(disp->save_pending) |= 8;
            ui_enter_mode(1);
            return;
        }
        if ((button_state_raw & KEY_ESC) && ui_esc_held > 0x27) {
            menu_back();
            return;
        }
        if (KEY(KEY_ESC) && --menu_value == 0xFFFF)
            menu_back();
        return;
    case 3:                                     /* maximum speed, acceleration, braking, volume */
    case 4:
    case 5:
    case 6:
        i = menu_item - 3;
        if (i == 0)
            menu_value = ui_knob_adjust(menu_value, 0xFF, 1, 0x0D);
        else if (i == 3)
            menu_value = ui_knob_adjust(menu_value, 0xFF, 0, 0x0D);
        else
            menu_value = ui_knob_adjust(menu_value, 0x40, 0, 5);
        if (KEY(KEY_OK | KEY_KNOB)) {
            (&ENTRY(lok_current_slot)->vmax)[i] = (u8)menu_value;
            disp = DISP(lok_current_slot);
            LO8(disp->track_pending) |= param_bit[i];
            LO8(disp->can_pending) |= param_bit[i];
            LO8(disp->save_pending) |= param_bit[i];
            ui_enter_mode(1);
        } else if (KEY(KEY_ESC)) {
            menu_back();
        }
        return;
    case 7:
        if (KEY(KEY_ESC)) {
            menu_back();
            return;
        }
        if (menu_sub == 1) {
            e = ENTRY(lok_current_slot);
            menu_value = ui_knob_adjust(menu_value, lok_cv_range[e->protocol][1], lok_cv_range[e->protocol][0], 4);
            if (KEY(KEY_OK | KEY_KNOB))
                menu_sub++;
        } else if (menu_sub == 2) {
            e = ENTRY(lok_current_slot);
            menu_value2 = ui_knob_adjust(menu_value2, lok_cv_value_range[e->protocol][1],
                                                      lok_cv_value_range[e->protocol][0], 4);
            if (KEY(KEY_OK | KEY_KNOB)) {
                disp = DISP(lok_current_slot);
                if (disp->prog_cv != 0)
                    return;
                disp->prog_cv = menu_value;
                disp->prog_value = (u8)menu_value2;
                HI8(disp->track_pending) |= 1;
                HI8(disp->can_pending) |= 1;
                ui_enter_mode(1);
            }
        }
        return;
    case 8:
        if (KEY(KEY_OK | KEY_KNOB)) {
            disp = DISP(slot);
            HI8(disp->track_pending) |= 0x40;
            HI8(disp->can_pending) |= 0x40;
            ui_enter_mode(1);
        } else if (KEY(KEY_ESC)) {
            menu_back();
        }
        return;
    default:
        return;
    }
}

/* menu_top 2, no item entered yet: step through the items the locomotive offers */
static void change_choose_item(u8 slot)
{
    s16 d = ui_knob_adjust(0, 1, -1, 1);
    u8 item;

    if (d != 0) {
        item = menu_item;
        do {
            item += (u8)d;
            menu_item = item;
            if (item == 0)
                menu_item = item = 8;
            if (item > 8)
                menu_item = item = 1;
        } while (!change_item_offered(item, lok_current_slot));
    }
    if (KEY(KEY_OK | KEY_KNOB)) {
        menu_enter();
        slot = lok_current_slot;
        switch (menu_item) {
        case 2:
            menu_value = 0;
            utf8_to_charset(ENTRY(slot)->name, lok_name_edit_buf, 0x10);
            if (lok_name_edit_buf[0] == 0) {
                lok_name_edit_buf[0] = 'A';
                lok_name_edit_buf[1] = 0;
            }
            break;
        case 3:
        case 4:
        case 5:
        case 6:
            menu_value = (&ENTRY(slot)->vmax)[menu_item - 3];
            break;
        case 7:
            menu_value = 1;
            menu_value2 = 0;
            break;
        }
        return;
    }
    if (KEY(KEY_ESC))
        menu_back();
}

static void menus(void)
{
    u8 item = menu_item;
    u8 sub, slot, t;
    lok_state_t *disp;

    if (item == 0) {
        menu_top = (u8)ui_knob_adjust(menu_top, 4, is_master != 0 ? 1 : 4, 0);
        if (porta_buttons_raw == 0x66)
            ui_enter_mode(4);
        if (KEY(KEY_OK | KEY_KNOB)) {
            menu_enter();
            if ((s8)lok_current_slot >= 0 && menu_top == 2 && ENTRY(lok_current_slot)->protocol == LOK_MFX)
                menu_item = 2;
        }
        if (KEY(KEY_ESC))
            ui_enter_mode(1);
        return;
    }
    switch (menu_top) {
    case 1:                                     /* new locomotive */
        sub = menu_sub;
        if (sub == 0) {
            menu_item = (u8)ui_knob_adjust(item, 2, 1, sub);
            if (KEY(KEY_ESC))
                menu_back();
            else if (KEY(KEY_OK | KEY_KNOB))
                menu_enter();
            return;
        }
        if (item == 1)
            new_from_database(sub);
        else if (item == 2)
            new_by_address();
        return;
    case 2:                                     /* change locomotive */
        slot = lok_current_slot;
        if ((s8)slot < 0) {
            menu_back();
            show_message(6, 0x3E);
            return;
        }
        sub = menu_sub;
        if (sub == 0) {
            change_choose_item(slot);
            return;
        }
        if (DISP(slot)->controller != mcan_sd_handle) {
            menu_back();
            show_message(4, 0x3E);
            return;
        }
        change_item(slot, sub);
        return;
    case 3:                                     /* delete locomotive */
        slot = lok_current_slot;
        if ((s8)slot < 0) {
            menu_back();
            show_message(6, 0x3E);
            return;
        }
        disp = DISP(slot);
        if ((disp->speed & 0x7F) != 0 || disp->controller != mcan_sd_handle) {
            menu_back();
            show_message(4, 0x3E);
            return;
        }
        if (KEY(KEY_ESC)) {
            menu_back();
            return;
        }
        if (KEY(KEY_OK | KEY_KNOB)) {
            lok_release_current();
            slot = lok_current_slot;
            ENTRY(slot)->unused = 0xFF;
            disp = DISP(slot);
            disp->flags |= 0x80;
            HI8(disp->track_pending) |= 0x10;
            HI8(disp->save_pending) |= 0x10;
            HI8(disp->can_pending) |= 0x10;
            ui_enter_mode(1);
        }
        return;
    case 4:                                     /* options */
        sub = menu_sub;
        if (sub == 0) {
            menu_item = (u8)ui_knob_adjust(item, 3, 1, sub);
            if (KEY(KEY_OK | KEY_KNOB)) {
                menu_enter();
                if (menu_item == 1)
                    menu_sub = language_index + 1;
            }
            if (KEY(KEY_ESC))
                menu_back();
            return;
        }
        switch (item) {
        case 1:                                 /* language */
            t = (u8)ui_knob_adjust(sub, 4, 1, 0);
            menu_sub = t;
            if (KEY(KEY_OK | KEY_KNOB)) {
                language_index = t - 1;
                ee_save_pending |= 0x20;
                ui_enter_mode(1);
            } else if (KEY(KEY_ESC)) {
                menu_back();
            }
            return;
        case 2:                                 /* information */
            menu_sub = (u8)ui_knob_adjust(sub, 9, 1, 0);
            if (KEY(KEY_ESC | KEY_OK | KEY_KNOB)) {
                menu_back();
                if (KEY(KEY_OK | KEY_KNOB))
                    ui_enter_mode(1);
            }
            return;
        case 3:                                 /* reset */
            if (KEY(KEY_ESC))
                menu_back();
            if (KEY(KEY_OK)) {
                erase_and_restart(EEPROM_BASE_ADDR);
                return;
            }
            if (porta_buttons_raw == 0x11) {
                IRQ_DISABLE();
                TIE &= ~0x10;
                TRACK_OUTPUT_OFF();
                eeprom_erase_sector(EE_MFX_REG_COUNTER);
                delay_short(0x20);
                eeprom_program_word(EE_MFX_REG_COUNTER, mfx_reg_counter + 1);
                eeprom_program_word(EE_MFX_REG_COUNTER + 2, 0);
                crt_restart();
            } else if (porta_buttons_raw == 0x88) {
                enter_bootloader();
            }
            return;
        default:
            return;
        }
    default:
        return;
    }
}

/* 0x95A0 */
void ui_task(void)
{
    u8 st, t, k, sel, keys;
    lok_state_t *d;

    encoder_clicks_rec_hi = encoder_clicks_hi;
    encoder_clicks_rec_lo = HI8(encoder_clicks_lo);
    if (operating_mode != 0) {
        if (KEY(KEY_STOP)) {
            system_status_flags ^= 1;
            mcan_status_pending |= 1;
            for (d = DISP(0); d <= DISP(9); d++)
                d->track_functions = 0;
        }
        if ((system_status_flags & 1) && ui_stop_held == 0x2E)
            mcan_status_pending |= 0x200;
        st = system_status_flags;
        if (st & 8) {
            if (!(st & 0x80)) {
                show_message(5, 0);
                st = system_status_flags;
            }
        } else if (message_id == 5) {
            st &= 0x7F;
            system_status_flags = st;
        }
        if ((st & 0x20) && !(st & 0x80)) {
            show_message(7, 0);
            st = system_status_flags;
        }
        if (st & 0x10) {
            if (!(st & 0x80))
                show_message(1, 0);
        } else if (message_id == 1) {
            system_status_flags = st & 0x7F;
            message_id = 0;
            ui_enter_mode(0);
        }
    }

    st = system_status_flags;
    if ((s8)st < 0) {                           /* a message: until its time or a key */
        t = message_ticks;
        if (t == 0)
            return;
        if ((porta_buttons_raw & porta_buttons_edge) || (button_state_edge & button_state_raw & 0xF7) ||
            (message_ticks = t - 1) == 0)
            system_status_flags = st & 0x7F;
        return;
    }

    switch (operating_mode) {
    case 0:                                     /* start-up */
        if ((button_state_raw & (KEY_STOP | KEY_KNOB)) == (KEY_STOP | KEY_KNOB) && ++track_fault_timer > 0x1E)
            erase_and_restart(EEPROM_BASE_ADDR);
#if MS1_RELEASE == 0x4010
        if (--menu_top == 0) {                  /* no current zero to measure */
            if (is_master != 0) {
#else
        t = menu_top;
        if (t <= 0x20)
            track_current_offset += track_current;
        menu_top = --t;
        if (t == 0) {
            if (is_master != 0) {
                track_current_offset >>= 5;
                if (track_current_offset > 0xFA)
                    track_current_offset = 0;
#endif
                track_fault_timer = 0;
                system_status_flags &= ~1;
                mcan_status_pending |= 1;
            }
            ui_enter_mode(1);
        }
        return;
    case 1:
        drive();
        return;
    case 2:                                     /* choose a locomotive */
        t = (u8)ui_knob_adjust(0, 1, -1, 1);
        if (t != 0)
            lok_current_slot = (u8)lok_next_used_slot((s8)lok_current_slot, (s8)t);
        if (!KEY(KEY_ESC | KEY_OK | KEY_KNOB))
            return;
        if (KEY(KEY_ESC) && ENTRY(menu_value)->unused == 0)
            lok_current_slot = (u8)menu_value;
        ui_enter_mode(1);
        return;
    case 3:
        menus();
        return;
    case 4:                                     /* EEPROM records */
        sel = (u8)ui_knob_adjust(menu_top, 3, 1, 0);
        menu_top = sel;
        keys = porta_buttons_raw & porta_buttons_edge;
        for (k = 1; k <= 3; k++, keys >>= 1)
            if (keys & 1)
                sel = k;
        menu_top = sel;
        if (KEY(KEY_ESC | KEY_OK | KEY_KNOB))
            ui_enter_mode(1);
        return;
    default:
        return;
    }
}
