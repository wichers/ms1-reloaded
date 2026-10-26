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
static const u8 lok_icon_group_order[16] = {
    0x0A, 0x09, 0x06, 0x01, 0x02, 0x0F, 0x0E, 0x04, 0x05, 0x03, 0x08, 0x07, 0x0D, 0x0C, 0x0B, 0x10,
};
static const u8 lok_icon_group_keys[16][9] = {
    { 0x80, 0x00, 0x70, 0x22, 0x42, 0x13, 0x33, 0x53, 0x63 },   /* group 1 */
    { 0x71, 0x80, 0x00, 0x22, 0x42, 0x13, 0x33, 0x53, 0x63 },   /* group 2 */
    { 0x62, 0x01, 0x72, 0x30, 0x10, 0x20, 0x42, 0x53, 0x83 },   /* group 3 */
    { 0x50, 0x40, 0x72, 0x30, 0x10, 0x20, 0x03, 0x63, 0x83 },   /* group 4 */
    { 0x32, 0x41, 0x72, 0x10, 0x20, 0x03, 0x53, 0x63, 0x83 },   /* group 5 */
    { 0x51, 0x22, 0x42, 0x03, 0x13, 0x33, 0x63, 0x73, 0x83 },   /* group 6 */
    { 0x02, 0x21, 0x42, 0x13, 0x33, 0x53, 0x63, 0x73, 0x83 },   /* group 7 */
    { 0x31, 0x11, 0x22, 0x42, 0x03, 0x53, 0x63, 0x73, 0x83 },   /* group 8 */
    { 0x61, 0x22, 0x42, 0x03, 0x13, 0x33, 0x53, 0x73, 0x83 },   /* group 9 */
    { 0x60, 0x22, 0x42, 0x03, 0x13, 0x33, 0x53, 0x73, 0x83 },   /* group 10 */
    { 0x42, 0x22, 0x03, 0x13, 0x33, 0x53, 0x63, 0x73, 0x83 },   /* group 11 */
    { 0x72, 0x30, 0x10, 0x20, 0x42, 0x03, 0x53, 0x63, 0x83 },   /* group 12 */
    { 0x10, 0x20, 0x72, 0x30, 0x42, 0x03, 0x53, 0x63, 0x83 },   /* group 13 */
    { 0x00, 0x70, 0x80, 0x22, 0x42, 0x13, 0x33, 0x53, 0x63 },   /* group 14 */
    { 0x70, 0x00, 0x80, 0x22, 0x42, 0x13, 0x33, 0x53, 0x63 },   /* group 15 */
    { 0x83, 0x03, 0x13, 0x23, 0x33, 0x43, 0x53, 0x63, 0x73 },   /* group 16 */
};

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
