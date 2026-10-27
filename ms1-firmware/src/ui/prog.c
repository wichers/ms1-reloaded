/*==========================================================================
 * ui/prog.c - the programming items of LOK ÄNDERN (menu_top 2) beyond the original's, in
 * page $3C (include/page3c.h)
 *
 * The items (menu_item, after SPEICHERN, CHANGE_SAVE), on the master for MM and DCC
 * locomotives:
 *   PROG_READ_CV  CV LESEN: a CV read in service mode (DCC), CV after CV
 *   PROG_READ     LESEN: the decoder's address and speed steps read in service mode (DCC):
 *                 CV29, then CV1 or CV17/CV18; the locomotive takes them
 *   PROG_SYMBOL   SYMBOL: the locomotive symbol, shown as the knob turns
 *   PROG_KEYS     TASTEN: which function a key switches, with which icon (key table below)
 *   PROG_STEPS    FAHRSTUFEN: 128, 28 or 14 speed steps (DCC)
 * Service mode first asks "PROG LOK?": only the decoder to program may be on the track. Each
 * job enters and leaves service mode on its own (dcc_svc.auto_end); meanwhile the display shows
 * PROG and the keys wait. menu_sub 1 sets an item up; the others are its steps.
 *
 * The keys: the 8 function keys sit next to the display's function icons, the light key next
 * to the light icon. prog_keys gives per key its input (PORTA bit, or KEY_LIGHT), its place,
 * and the icons of the glass beside it (lcd_icons.h: the key's group in lcd_icon_segments, a
 * variant per entry), each with the function symbol (lok_function_t.icon) it stands for. The
 * last variant, PROG_NO_ICON, has no segments: a function on the key without an icon. A
 * function's key byte is key << 4 | variant, as lok_assign_functions writes it.
 *
 * Functions:
 *   prog_offered  - whether an item is offered for the slot
 *   prog_item     - the item's controls, every UI pass
 *   prog_text     - the item's text line, and the selected key's icon blinking
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/lokstack.h"
#include "../include/page3c.h"
#include "../include/dcc.h"
#include "../include/rom_tables.h"
#include "../include/str.h"
#include "../include/ui.h"

#define KEY(mask)       (button_state_edge & button_state_raw & (mask))
#define KEY_ESC         0x01
#define KEY_OK          0x02
#define KEY_LIGHT       0x04
#define KEY_KNOB        0x10

#define PROG_NO_ICON    3
#define NO_FUNCTION     0xFF
#define SYMBOL_NONE     0x10            /* the function symbol of a function without an icon */

/* The keys beside the function icons. The PORTA bits' places come from the manual (the keys
 * left of the display are F1-F4 from the top for a locomotive without symbols); the icons of
 * the variants from lcd_icons.h, their symbols from the original's key preferences
 * (lok_functions.c: the symbol whose first choice the variant is, else whose choice it is). */
static const struct {
    u8 port;                            /* PORTA bit, or 8 the light key (PB2) */
    u8 symbol[3];                       /* per icon variant its function symbol; 0: none */
} prog_keys[9] = {
    { 0, { 14, 3, 7 } },                /* left, top: light, bell, slow speed (turtle) */
    { 1, { 13, 8, 0 } },                /* left, 2nd: sound (speaker), ramp */
    { 2, { 13, 7, 11 } },               /* left, 3rd: speaker, turtle, function */
    { 3, { 12, 8, 5 } },                /* left, bottom: music, ramp, light */
    { 4, { 4, 5, 11 } },                /* right, top: horn, light, function */
    { 5, { 4, 6, 0 } },                 /* right, 2nd: horn, coupler (Telex) */
    { 6, { 10, 9, 3 } },                /* right, 3rd: steam, hook, bell */
    { 7, { 15, 2, 12 } },               /* right, bottom: light 2, light, music */
    { 8, { 1, 0, 0 } },                 /* the light key: light */
};

/* the item texts, per language */
#define T(s)    ((const u8 *)(s))
enum { TX_READ_CV, TX_READ, TX_SYMBOL, TX_KEYS, TX_STEPS, TX_KEY, TX_COUNT };
static const u8 *const prog_texts[4][TX_COUNT] = {
    { T("READ CV"), T("READ"), T("SYMBOL"), T("KEYS"), T("SPEED STEPS"), T("KEY?") },
    { T("CV LESEN"), T("LESEN"), T("SYMBOL"), T("TASTEN"), T("FAHRSTUFEN"), T("TASTE?") },
    { T("LIRE CV"), T("LIRE"), T("SYMBOLE"), T("TOUCHES"), T("CRANS"), T("TOUCHE?") },
    { T("CV LEZEN"), T("LEZEN"), T("SYMBOOL"), T("TOETSEN"), T("RIJSTAPPEN"), T("TOETS?") },
};
#define TEXT(k)  (prog_texts[language_index][k])

static const u8 steps_flags[3] = { 0, LOKP_DCC_28, LOKP_DCC_14 };
static const u8 steps_number[3] = { 128, 28, 14 };

u16 prog_offered(u16 item, u16 slot)
{
    const lok_record_t *e = &lok_records[slot];

    if (is_master == 0 || e->kinds[0] != 0x80)
        return 0;
    switch (item) {
    case PROG_READ_CV:
    case PROG_READ:
    case PROG_STEPS:
        return e->protocol == LOK_DCC;
    case PROG_SYMBOL:
    case PROG_KEYS:
        return e->protocol == LOK_DCC || e->protocol == LOK_MM;
    default:
        return 0;
    }
}

/* the slot's record changed: store it, tell CAN */
static void changed(u16 slot)
{
    LO8(lok_states[slot].save_pending) |= 8;
    LO8(lok_states[slot].can_pending) |= 8;
}

/* the CAN object numbers of the functions with an icon, after the speed object, as
 * lok_assign_functions numbers them (its key placement would undo the user's) */
static void number_objects(u16 slot)
{
    lok_record_t *e = &lok_records[slot];
    u8 k, object = e->speed_object;

    for (k = 0; k < 16; k++)
        e->functions[k].object = e->functions[k].icon != 0 ? ++object : 0;
}

/* the function on key (0-8), or NO_FUNCTION */
static u8 function_on(u16 slot, u8 key)
{
    const lok_function_t *f = lok_records[slot].functions;
    u8 k;

    for (k = 0; k < 16; k++)
        if (f[k].icon != 0 && f[k].key >> 4 == key)
            return k;
    return NO_FUNCTION;
}

/* function fn (or NO_FUNCTION) onto key with icon variant v, momentary: the function that was
 * there loses its key, and fn its old one */
static void put_function(u16 slot, u8 key, u8 fn, u8 v, u8 momentary)
{
    lok_function_t *f = lok_records[slot].functions;
    u8 old = function_on(slot, key), symbol;

    if (old != NO_FUNCTION)
        f[old].icon = 0;
    if (fn != NO_FUNCTION) {
        symbol = v == PROG_NO_ICON ? SYMBOL_NONE : prog_keys[key].symbol[v];
        f[fn].icon = symbol | (momentary ? 0x80 : 0);
        f[fn].key = key << 4 | v;
    }
    number_objects(slot);
    changed(slot);
}

/* the next icon variant beside key after v: the glass's, then none */
static u8 next_variant(u8 key, u8 v)
{
    do
        v = v == PROG_NO_ICON ? 0 : v + 1;
    while (v != PROG_NO_ICON && prog_keys[key].symbol[v] == 0);
    return v;
}

/* the key pressed now (0-8), or NO_FUNCTION */
static u8 key_pressed(void)
{
    u8 k, keys = porta_buttons_edge & porta_buttons_raw;

    for (k = 0; k < 8; k++)
        if ((keys >> prog_keys[k].port) & 1)
            return k;
    return KEY(KEY_LIGHT) ? 8 : NO_FUNCTION;
}

/* service mode after PROG LOK?: menu_sub 2 asks; OK goes on to sub 3, ESC back */
static u8 confirmed(void)
{
    if (KEY(KEY_OK | KEY_KNOB)) {
        menu_sub = 3;
        return 1;
    }
    if (KEY(KEY_ESC))
        ui_enter_mode(1);
    return 0;
}

static void read_cv(u16 cv)
{
    dcc_service_start(SVC_READ, cv, 0);
    dcc_svc.auto_end = 1;
}

static void read_item(u16 slot, u8 sub)
{
    lok_record_t *e = &lok_records[slot];
    u16 st;

    switch (sub) {
    case 1:
        menu_sub = 2;
        return;
    case 2:
        if (confirmed())
            read_cv(29);
        return;
    }
    st = dcc_service_status();
    if ((u8)st == SVC_BUSY)
        return;
    if ((u8)st != SVC_OK) {
        ui_enter_mode(1);
        return;
    }
    switch (sub) {
    case 3:                             /* CV29 read: the address CVs next */
        menu_value2 = st >> 8;
        menu_sub = 4;
        read_cv(menu_value2 & 0x20 ? 17 : 1);
        return;
    case 4:
        menu_value = st >> 8;
        if (!(menu_value2 & 0x20))
            break;
        menu_sub = 5;
        read_cv(18);
        return;
    case 5:
        menu_value = (menu_value & 0x3F) << 8 | (st >> 8);
        break;
    default:
        return;
    }
    /* the decoder's address and speed steps into the locomotive */
    e->address = menu_value;
    e->protocol_flags = (e->protocol_flags & ~(LOKP_DCC_STEPS | LOKP_DCC_LONG))
                        | (menu_value2 & 0x02 ? 0 : LOKP_DCC_14)
                        | ((menu_value2 & 0x20) && menu_value <= 127 ? LOKP_DCC_LONG : 0);
    LO8(lok_states[slot].save_pending) |= 4;
    LO8(lok_states[slot].can_pending) |= 4;
    LO8(lok_states[slot].track_pending) |= 1;
    changed(slot);
    lok_check_conflict(slot);
    lok_recheck_conflicts();
    ui_enter_mode(1);
}

static void read_cv_item(u8 sub)
{
    u16 st;

    switch (sub) {
    case 1:
        menu_value = 1;
        menu_sub = 2;
        return;
    case 2:
        confirmed();
        return;
    case 3:                             /* the CV */
        menu_value = ui_knob_adjust(menu_value, 1024, 1, 4);
        if (KEY(KEY_OK | KEY_KNOB)) {
            read_cv(menu_value);
            menu_sub = 4;
        } else if (KEY(KEY_ESC)) {
            ui_enter_mode(1);
        }
        return;
    case 4:                             /* reading */
        st = dcc_service_status();
        if ((u8)st == SVC_BUSY)
            return;
        menu_value2 = st;
        menu_sub = (u8)st == SVC_OK ? 5 : 3;
        return;
    default:                            /* its value; the knob for another CV */
        if (ui_knob_adjust(0, 1, -1, 1) != 0 || KEY(KEY_OK | KEY_KNOB))
            menu_sub = 3;
        else if (KEY(KEY_ESC))
            ui_enter_mode(1);
        return;
    }
}

static void keys_item(u16 slot, u8 sub)
{
    lok_function_t *f = lok_records[slot].functions;
    u8 k = key_pressed(), fn, v;
    s16 d;

    if (sub == 1) {
        menu_value = NO_FUNCTION;       /* the selected key */
        menu_sub = 2;
        return;
    }
    if (k != NO_FUNCTION) {
        menu_value = k;
        return;
    }
    if (KEY(KEY_ESC)) {
        ui_enter_mode(1);
        return;
    }
    if (menu_value == NO_FUNCTION)
        return;
    k = (u8)menu_value;
    fn = function_on(slot, k);
    v = fn == NO_FUNCTION ? PROG_NO_ICON : f[fn].key & 0x0F;
    d = ui_knob_adjust(0, 1, -1, 1);
    if (d != 0) {                       /* the function: none, F0-F15 */
        fn = fn == NO_FUNCTION ? (d > 0 ? 0 : 15) : (u8)(fn + d);
        if (fn > 15)
            fn = NO_FUNCTION;
        put_function(slot, k, fn, v, 0);
    } else if (fn != NO_FUNCTION && KEY(KEY_KNOB)) {
        put_function(slot, k, fn, next_variant(k, v), f[fn].icon & 0x80);
    } else if (fn != NO_FUNCTION && KEY(KEY_OK)) {
        put_function(slot, k, fn, v, !(f[fn].icon & 0x80));
    }
}

u16 prog_item(u16 slot, u16 sub)
{
    lok_record_t *e = &lok_records[slot];
    u8 i;

    switch (menu_item) {
    case PROG_READ_CV:
        read_cv_item((u8)sub);
        break;
    case PROG_READ:
        read_item(slot, (u8)sub);
        break;
    case PROG_SYMBOL:
        if (sub == 1) {
            menu_value2 = e->symbol;    /* back to it on ESC */
            menu_sub = 2;
        }
        e->symbol = (u8)ui_knob_adjust(e->symbol, 3, 0, 0);
        if (KEY(KEY_OK | KEY_KNOB)) {
            changed(slot);
            ui_enter_mode(1);
        } else if (KEY(KEY_ESC)) {
            e->symbol = (u8)menu_value2;
            ui_enter_mode(1);
        }
        break;
    case PROG_KEYS:
        keys_item(slot, (u8)sub);
        break;
    case PROG_STEPS:
        if (sub == 1) {
            for (i = 0; i < 2 && steps_flags[i] != (e->protocol_flags & LOKP_DCC_STEPS); i++)
                ;
            menu_value = i;
            menu_sub = 2;
        }
        menu_value = ui_knob_adjust(menu_value, 2, 0, 0);
        if (KEY(KEY_OK | KEY_KNOB)) {
            e->protocol_flags = (e->protocol_flags & ~LOKP_DCC_STEPS) | steps_flags[menu_value];
            changed(slot);
            LO8(lok_states[slot].track_pending) |= 1;
            ui_enter_mode(1);
        } else if (KEY(KEY_ESC)) {
            ui_enter_mode(1);
        }
        break;
    }
    return 0;
}

/* the item's text into buf: its name (menu_sub 0), else its value; the selected key's icon
 * blinks (TASTEN) */
u16 prog_text(u16 slot, u8 *buf)
{
    const lok_function_t *f = lok_records[slot].functions;
    u8 item = menu_item, sub = menu_sub, fn, v;

    if (sub == 0) {
        str_format(buf, (const u8 *)"%s", TEXT(item - PROG_READ_CV));
        return 0;
    }
    switch (item) {
    case PROG_READ_CV:
        if (sub == 2)
            str_format(buf, (const u8 *)"%s", LANG(0x20));
        else if (sub == 5)
            str_format(buf, (const u8 *)"CV%d=%d", menu_value, menu_value2 >> 8);
        else
            str_format(buf, (const u8 *)"CV%d", menu_value);
        break;
    case PROG_READ:
        str_format(buf, (const u8 *)"%s", sub == 2 ? LANG(0x20) : TEXT(TX_READ));
        break;
    case PROG_SYMBOL:
        str_format(buf, (const u8 *)"%s", LANG(0x1E));
        break;
    case PROG_STEPS:
        str_format(buf, (const u8 *)"%d", steps_number[menu_value]);
        break;
    case PROG_KEYS:
        if (menu_value == NO_FUNCTION) {
            str_format(buf, (const u8 *)"%s", TEXT(TX_KEY));
            break;
        }
        fn = function_on(slot, (u8)menu_value);
        if (fn == NO_FUNCTION)
            str_format(buf, (const u8 *)"--");
        else
            str_format(buf, (const u8 *)"F%d%s", fn, (f[fn].icon & 0x80) ? " M" : "");
        /* the key's icon off in the blink's off phase */
        v = fn == NO_FUNCTION ? PROG_NO_ICON : f[fn].key & 0x0F;
        if (v != PROG_NO_ICON && !((ms_tick >> 8) & 3)) {
            const u8 *seg = &lcd_icon_segments[(menu_value * 4 + v) * 4];
            lcd_buf[seg[0]] &= ~seg[1];
            lcd_buf[seg[2]] &= ~seg[3];
        }
        break;
    }
    return 0;
}
