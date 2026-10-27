/*==========================================================================
 * ui/accessory.c - DCC accessory devices: the ZUBEHÖR menu, accessory mode, their packets,
 * in page $3C (include/page3c.h)
 *
 * An accessory device (acc_dev, include/accessory.h) is ACC_ROWS turnouts on consecutive
 * output addresses, from the device's first; the device library keeps its name, address and
 * the icon beside each function key (lok/library.c).
 *
 * ZUBEHÖR (menu_top 5, on the master; menus in ui/ui_control.c call accessory_menu):
 *   the list (menu_sub 0)  the library's devices, newest first, then NEU; OK uses a device
 *                          (accessory mode), a knob push edits it or a new one, ESC goes back
 *   the editor (menu_sub 1) pages, the knob to choose, OK or a knob push to enter, ESC back:
 *                          ADRESSE (the first output address, 1-2044), NAME, TASTEN (a function
 *                          key, then a knob push for the next icon beside it), PROG (the
 *                          decoder's address in service mode after PROG LOK?: CV1 and CV9 of
 *                          decoder address (address - 1) / 4 + 1), SPEICHERN, LÖSCHEN
 * Accessory mode (operating mode 6, accessory_task): row r of the function keys switches
 * turnout address + r - the left key output 0, the right key output 1 (ui/prog.c's key
 * table: PA0-PA3 left, PA4-PA7 right); the knob goes to the next device, ESC to the list, OK
 * to the locomotive. The icon beside the key last pressed in a row is on.
 *
 * Packets (RCN-213): 10AAAAAA 1aaaCPPO, decoder address A (aaa its bits 6-8 complemented),
 * port P, output O, C on: per switching ACC_ON_PACKETS with C set, then after ACC_ON_MS one
 * with C clear (dcc_accessory_next, from track_scheduler while acc_pending).
 *
 * Functions:
 *   accessory_menu      - the ZUBEHÖR menu's controls
 *   accessory_task      - accessory mode's controls
 *   accessory_text      - the text line and the key icons of both
 *   dcc_accessory_next  - the next accessory packet
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/lokstack.h"
#include "../include/page3c.h"
#include "../include/dcc.h"
#include "../include/accessory.h"
#include "../include/rom_tables.h"
#include "../include/str.h"
#include "../include/ui.h"

#define KEY(mask)       (button_state_edge & button_state_raw & (mask))
#define KEY_ESC         0x01
#define KEY_OK          0x02
#define KEY_KNOB        0x10

#define ACC_ON_PACKETS  3
#define ACC_ON_MS       200
#define NO_ICON         3
#define NO_KEY          0xFF

enum { PG_ADDRESS, PG_NAME, PG_KEYS, PG_PROG, PG_SAVE, PG_DELETE, PG_COUNT };

#define T(s)    ((const u8 *)(s))
enum { TX_MENU, TX_NEW, TX_ADDRESS, TX_NAME, TX_KEYS, TX_PROG, TX_SAVE, TX_DELETE, TX_KEY, TX_COUNT };
static const u8 *const acc_texts[4][TX_COUNT] = {
    { T("ACCESSORY"), T("NEW"), T("ADDRESS"), T("NAME"), T("KEYS"), T("PROG"), T("SAVE"), T("DELETE"), T("KEY?") },
    { T("ZUBEHoR"), T("NEU"), T("ADRESSE"), T("NAME"), T("TASTEN"), T("PROG"), T("SPEICHERN"), T("LoSCHEN"), T("TASTE?") },
    { T("ACCESSOIRE"), T("NOUVEAU"), T("ADRESSE"), T("NOM"), T("TOUCHES"), T("PROG"), T("SAUVER"), T("SUPPR"), T("TOUCHE?") },
    { T("TOEBEHOREN"), T("NIEUW"), T("ADRES"), T("NAAM"), T("TOETSEN"), T("PROG"), T("OPSLAAN"), T("WISSEN"), T("TOETS?") },
};
#define TEXT(k)  (acc_texts[language_index][k])

/* the PORTA bit of function key k, PA0-PA3 the left rows, PA4-PA7 the right */
#define KEY_BIT(k)  (k)

/* the key pressed now (0-7), or NO_KEY */
static u8 key_pressed(void)
{
    u8 k, keys = porta_buttons_edge & porta_buttons_raw;

    for (k = 0; k < 8; k++)
        if ((keys >> KEY_BIT(k)) & 1)
            return k;
    return NO_KEY;
}

/* ---- the packets ---- */

static void switch_turnout(u8 row, u8 output)
{
    acc_cmd_t *c, *free = 0;
    u16 address = acc_dev.address + row;
    u8 i;

    if (address > ACC_MAX_ADDRESS)
        return;
    for (i = 0; i < ACC_CMDS; i++) {
        c = &acc_cmds[i];
        if (c->address == address)
            break;
        if (c->address == 0 && free == 0)
            free = c;
    }
    if (i == ACC_CMDS) {
        if (free == 0)
            return;
        c = free;
        acc_pending++;
    }
    c->address = address;
    c->output = output;
    c->left = ACC_ON_PACKETS;
    acc_dev.state = (acc_dev.state & ~(1 << row)) | output << row;
    acc_dev.known |= 1 << row;
}

/* the packet of command c, on or off; 0 when an idle packet went first */
static u8 accessory_packet(const acc_cmd_t *c, u8 on)
{
    u16 d = (c->address - 1) / 4 + 1;
    u8 p[2];

    p[0] = 0x80 | (u8)(d & 0x3F);
    p[1] = 0x80 | (u8)((~d >> 6) & 7) << 4 | on << 3 | (u8)((c->address - 1) & 3) << 1 | c->output;
    return dcc_packet(p, 2, 2);
}

u16 dcc_accessory_next(void)
{
    acc_cmd_t *c;
    u8 i;

    for (i = 0; i < ACC_CMDS; i++) {
        c = &acc_cmds[i];
        if (c->address == 0)
            continue;
        if (c->left) {
            if (accessory_packet(c, 1)) {
                c->left--;
                c->since = ms_tick;
            }
            return 1;
        }
        if ((u16)(ms_tick - c->since) >= ACC_ON_MS) {
            if (accessory_packet(c, 0)) {
                c->address = 0;
                acc_pending--;
            }
            return 1;
        }
    }
    return 0;
}

/* ---- the menu and the editor ---- */

static void new_device(void)
{
    u8 k;

    acc_dev.id = 0;
    acc_dev.name[0] = 0;
    acc_dev.address = 1;
    for (k = 0; k < 8; k++)
        acc_dev.icons[k] = NO_ICON;
    acc_dev.state = acc_dev.known = 0;
}

static void list(void)
{
    u16 n = library_count(LIB_ACCESSORY);

    menu_value = ui_knob_adjust(menu_value, n, 0, 0);
    if (KEY(KEY_ESC)) {
        menu_back();
        return;
    }
    if (!KEY(KEY_OK | KEY_KNOB))
        return;
    if (menu_value == n)
        new_device();
    else if (!library_load_accessory(menu_value))
        return;
    if (menu_value < n && KEY(KEY_OK)) {
        ui_enter_mode(6);
        return;
    }
    menu_sub = 1;
    acc_dev.page = PG_ADDRESS;
    acc_dev.editing = 0;
    ui_knob_reset();
}

static void edit_name(void)
{
    u8 *p = &lok_name_edit_buf[acc_dev.cursor];
    s16 y;

    *p = charset_index_map[ui_knob_adjust((u8)charset_index_lookup(*p), 0x4A, 0, 4)];
    if (KEY(KEY_KNOB) && acc_dev.cursor <= 13) {
        p = &lok_name_edit_buf[++acc_dev.cursor];
        if (*p == 0) {
            p[0] = 'A';
            p[1] = 0;
        }
    }
    if (KEY(KEY_OK)) {
        for (y = (s16)str_len(lok_name_edit_buf) - 1; y >= 0 && lok_name_edit_buf[y] == ' '; y--)
            lok_name_edit_buf[y] = 0;
        charset_to_utf8(lok_name_edit_buf, acc_dev.name, 0x10);
        acc_dev.editing = 0;
    } else if (KEY(KEY_ESC)) {
        if (acc_dev.cursor == 0)
            acc_dev.editing = 0;
        else
            acc_dev.cursor--;
    }
}

/* a page's start */
static void enter_page(void)
{
    acc_dev.editing = 1;
    acc_dev.cursor = 0;
    if (acc_dev.page == PG_NAME) {
        utf8_to_charset(acc_dev.name, lok_name_edit_buf, 0x10);
        if (lok_name_edit_buf[0] == 0) {
            lok_name_edit_buf[0] = 'A';
            lok_name_edit_buf[1] = 0;
        }
    } else if (acc_dev.page == PG_KEYS) {
        acc_dev.cursor = NO_KEY;
    }
}

static void editor(void)
{
    u8 k, r;

    if (!acc_dev.editing) {
        acc_dev.page = (u8)ui_knob_adjust(acc_dev.page, PG_COUNT - 1, 0, 0);
        if (KEY(KEY_OK | KEY_KNOB))
            enter_page();
        else if (KEY(KEY_ESC))
            menu_sub = 0;
        return;
    }
    switch (acc_dev.page) {
    case PG_ADDRESS:
        acc_dev.address = ui_knob_adjust(acc_dev.address, ACC_MAX_ADDRESS, 1, 5);
        if (KEY(KEY_OK | KEY_KNOB | KEY_ESC))
            acc_dev.editing = 0;
        return;
    case PG_NAME:
        edit_name();
        return;
    case PG_KEYS:
        k = key_pressed();
        if (k != NO_KEY)
            acc_dev.cursor = k;
        else if (KEY(KEY_OK | KEY_ESC))
            acc_dev.editing = 0;
        else if (acc_dev.cursor != NO_KEY && KEY(KEY_KNOB))
            acc_dev.icons[acc_dev.cursor] = prog_next_icon(acc_dev.cursor, acc_dev.icons[acc_dev.cursor]);
        return;
    case PG_PROG:
        if (acc_dev.cursor == 0) {          /* PROG LOK? */
            if (KEY(KEY_OK | KEY_KNOB)) {
                dcc_service_start(SVC_ACC_ADDRESS, (acc_dev.address - 1) / 4 + 1, 0);
                dcc_svc.auto_end = 1;
                acc_dev.cursor = 1;
            } else if (KEY(KEY_ESC)) {
                acc_dev.editing = 0;
            }
        } else if ((u8)dcc_service_status() != SVC_BUSY) {
            acc_dev.editing = 0;
        }
        return;
    case PG_SAVE:
        if (acc_dev.name[0] == 0)           /* no name: its address */
            str_format(acc_dev.name, (const u8 *)"A %d", acc_dev.address);
        r = (u8)library_save_accessory();
        if (r == LIB_AGAIN)
            return;
        if (r == LIB_FULL)
            show_message(2, 0x3E);
        else if (r == LIB_ERROR)
            show_message(LANG_ERROR - 0x16, 0x3E);
        acc_dev.editing = 0;
        if (r == LIB_DONE) {
            menu_sub = 0;
            menu_value = 0;                 /* newest first: the device saved */
        }
        return;
    case PG_DELETE:
        if (acc_dev.cursor == 0) {          /* LÖSCHEN? */
            if (KEY(KEY_OK | KEY_KNOB))
                acc_dev.cursor = 1;
            else if (KEY(KEY_ESC))
                acc_dev.editing = 0;
            return;
        }
        r = (u8)library_delete_accessory();
        if (r == LIB_AGAIN)
            return;
        acc_dev.editing = 0;
        menu_sub = 0;
        menu_value = 0;
        return;
    }
}

u16 accessory_menu(void)
{
    if (menu_sub == 0)
        list();
    else
        editor();
    return 0;
}

/* ---- accessory mode ---- */

u16 accessory_task(void)
{
    u16 n = library_count(LIB_ACCESSORY);
    s16 d;
    u8 k;

    if (n == 0) {
        ui_enter_mode(3);
        return 0;
    }
    k = key_pressed();
    if (k != NO_KEY)
        switch_turnout(k & 3, k >> 2);
    d = ui_knob_adjust(0, 1, -1, 1);
    if (d != 0)
        library_load_accessory((acc_dev.index + n + d) % n);
    if (KEY(KEY_ESC)) {
        ui_enter_mode(3);
        menu_top = 5;
        menu_item = 1;
        menu_sub = 0;
        menu_value = acc_dev.index;
    } else if (KEY(KEY_OK)) {
        ui_enter_mode(1);
    }
    return 0;
}

/* ---- the display ---- */

/* the function key icons: per key its variant, on when lit; the key at blink blinks */
static void draw_icons(u8 lit, u8 blink)
{
    const u8 *seg;
    u8 k, v;

    for (k = 0; k < 9; k++)
        for (v = 0; v < 4; v++) {
            seg = &lcd_icon_segments[(k * 4 + v) * 4];
            lcd_buf[seg[0]] &= ~seg[1];
            lcd_buf[seg[2]] &= ~seg[3];
        }
    for (k = 0; k < 8; k++) {
        v = acc_dev.icons[k];
        if (v >= NO_ICON || (k == blink && !((ms_tick >> 8) & 3)))
            continue;
        seg = &lcd_icon_segments[(k * 4 + v) * 4];
        if (seg[0] == 0 && seg[1] == 0)
            continue;
        lcd_buf[seg[0]] |= seg[1];
        if ((lit >> k) & 1)
            lcd_buf[seg[2]] |= seg[3];
    }
}

static void put_name(u8 *buf, const u8 *name)
{
    if (name[0] == 0)
        str_format(buf, (const u8 *)"A %d", acc_dev.address);
    else
        utf8_to_charset(name, buf, 0x10);
}

u16 accessory_text(u8 *buf)
{
    u8 lit = 0, r, n;

    if (operating_mode == 6) {
        for (r = 0; r < ACC_ROWS; r++)
            if ((acc_dev.known >> r) & 1)
                lit |= 1 << (r + ((acc_dev.state >> r) & 1) * 4);
        draw_icons(lit, NO_KEY);
        put_name(buf, acc_dev.name);
        return 0;
    }
    if (menu_item == 0) {
        str_format(buf, (const u8 *)"%s", TEXT(TX_MENU));
        return 0;
    }
    if (menu_sub == 0) {
        n = (u8)library_count(LIB_ACCESSORY);
        if (menu_value >= n) {
            str_format(buf, (const u8 *)"%s", TEXT(TX_NEW));
        } else {
            library_name(LIB_ACCESSORY, menu_value, lok_name_edit_buf);
            utf8_to_charset(lok_name_edit_buf, buf, 0x10);
        }
        return 0;
    }
    if (!acc_dev.editing) {
        str_format(buf, (const u8 *)"%s", TEXT(TX_ADDRESS + acc_dev.page));
        return 0;
    }
    switch (acc_dev.page) {
    case PG_ADDRESS:
        str_format(buf, (const u8 *)"A %d", acc_dev.address);
        break;
    case PG_NAME:                       /* the cursor character inverse */
        str_format(buf, (const u8 *)"%s", lok_name_edit_buf);
        if (buf[acc_dev.cursor] == ' ')
            buf[acc_dev.cursor] = 0xF9;
        else
            buf[acc_dev.cursor] |= 0x80;
        break;
    case PG_KEYS:
        draw_icons(0, acc_dev.cursor);
        str_format(buf, (const u8 *)"%s", TEXT(TX_KEY));
        break;
    case PG_PROG:
        str_format(buf, (const u8 *)"%s", LANG(0x20));
        break;
    case PG_SAVE:
        str_format(buf, (const u8 *)"%s", TEXT(TX_SAVE));
        break;
    case PG_DELETE:
        str_format(buf, (const u8 *)"%s?", TEXT(TX_DELETE));
        break;
    }
    return 0;
}
