/*==========================================================================
 * lok/lok_functions.c - a locomotive's functions: CAN objects and keys
 *
 * Functions:
 *   lok_assign_functions  (0x5C12)
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/lokstack.h"
#include "../include/mcan.h"

/* The icon groups (1-16) in the order they get keys, and per group the 9 keys it may have
 * (bits 4-7: PORTA bit, 8 = light), most wanted first. */
extern const u8 lok_icon_group_order[16];   /* 0xEB6D */
extern const u8 lok_icon_group_keys[16][9]; /* 0xEB7D */

/* The functions of slot's locomotive: the first 9 with an icon keep it, and as master each
 * gets a CAN object number after the speed object; as slave a function without icon that
 * had an object makes the master's answer for it count (mcan_request_advance). Then the
 * keys: group by group in lok_icon_group_order, each function of the group takes the first
 * of its group's keys still free; a function that finds none loses its icon. */
void lok_assign_functions(u16 slot)
{
    lok_function_t *f;
    const u8 *choice;
    u16 object = lok_records[slot].speed_object;
    u16 n = 0, used = 0, i, k;
    s16 group;
    u8 icon, key;

    for (f = lok_records[slot].functions, i = 0; i < 16; i++, f++) {
        if (f->icon != 0 && ++n > 9)
            f->icon = 0;
        f->key = 0;
        if (f->icon != 0) {
            if (is_master != 0)
                f->object = (u8)++object;
        } else {
            if (f->object != 0)
                mcan_request_advance(&lok_states[slot], 0x300 | f->object);
            f->object = 0;
        }
    }
    for (i = 0; i <= 15; i++) {
        group = (s16)lok_icon_group_order[i] - 1;
        for (f = lok_records[slot].functions, k = 0; k <= 15; k++, f++) {
            icon = f->icon;
            if ((s16)((icon & 0x1F) - 1) != group)
                continue;
            f->icon = 0;
            for (choice = lok_icon_group_keys[group]; choice != lok_icon_group_keys[group] + 9; choice++) {
                key = *choice;
                if (!((used >> (key >> 4)) & 1)) {
                    f->icon = icon;
                    f->key = key;
                    used |= 1u << (key >> 4);
                    break;
                }
            }
        }
    }
}
