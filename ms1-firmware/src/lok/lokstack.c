/*==========================================================================
 * lok/lokstack.c - lookups in the locomotive slots (lok_records, lok_states; lokstack.h)
 *
 * Functions:
 *   lok_slot_of_object        (0x59FB) - slot by CAN object id
 *   mcan_object_kind           (0x5A37) - object kind of a CAN object id
 *   mcan_kind_class             (0x5AC1) - category of an object kind
 *   mcan_object_class          (0x5AF4) - both
 *   mcan_object_change_count            (0x5AFA) - change counter of a slot's object
 *   mcan_object_controller        (0x5B42) - controller of a slot's object
 *   mcan_next_object (0x5B85) - next object id in lok_id_table
 *   lok_state_clear       (0x5D76) - clear a slot's state
 *   lok_take_current (0xD898) - the current slot is driven here
 *   lok_release_current(0xD8B6) - it no longer is
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/str.h"
#include "../include/lokstack.h"
#include "../include/rom_tables.h"


/* The slot whose state has the object id handle and whose record is in use, or 10. Slot 10,
 * the edit record, is looked at too. */
s16 lok_slot_of_object(u16 handle)
{
    s16 i;
    const lok_state_t *d = lok_states;
    const lok_record_t *r = lok_records;

    for (i = 0; i <= 10; i++, r++) {
        if ((d++)->object == handle && r->unused == 0)
            return i;
    }
    return 10;
}

/*--------------------------------------------------------------------------
 * mcan_object_kind - The kind of the CAN object with this handle (0x5A37)
 *
 * The object kinds (the M-CAN document's): 1 busmaster (handle 0), 2 directory service
 * (1), 3 central (2), 0x40 a device (0x100 this one as master, 0x101 the slave's), a
 * slot's first kind for 0x200-0x209 (lok_record_t.kinds: 0x80-0x82); 0 none. As slave the
 * handles the master gave out count (mcan_peer_handle, mcan_start_handle, mcan_sy_handle,
 * mcan_sd_handle) and slots are found by their state's object (lok_slot_of_object).
 *------------------------------------------------------------------------*/
u16 mcan_object_kind(u16 handle)
{
    u16 slot;
    lok_record_t *entry;

    if (is_master == 0) {
        /* slave: the handles the master gave out */
        if (handle == mcan_peer_handle) return 1;
        if (handle == mcan_start_handle) return 2;
        if (handle == mcan_sy_handle) return 3;
        if (handle == mcan_sd_handle) return 0x40;

        /* a slot */
        slot = lok_slot_of_object(handle);
        if (slot > 9) return 0;

        entry = &lok_records[slot];
        return (u16)entry->kinds[0];
    }

    /* master: the fixed handles */
    if (handle == 0) return 1;
    if (handle == 1) return 2;
    if (handle == 2) return 3;
    if (handle == 0x100) return 0x40;
    if (handle == 0x101) {
        if (mcan_peer_handle == 0) {
            /* no slave: a slot? */
        } else {
            return 0x40;
        }
    }

    /* a slot, 0x200-0x209 */
    slot = handle - 0x200;
    if (slot > 9) return 0;

    entry = &lok_records[slot];
    if (entry->unused != 0) return 0;

    return (u16)entry->kinds[0];
}

/*--------------------------------------------------------------------------
 * mcan_kind_class - The class of an object kind (0x5AC1): 1, 2, 3 for the kinds 1-3
 * (busmaster, directory, central), 4 for a device (0x40), 5 for a slot (0x80-0x82), else 0
 *------------------------------------------------------------------------*/
u16 mcan_kind_class(u8 kind)
{
    switch (kind) {
    case 1:    return 1;
    case 2:    return 2;
    case 3:    return 3;
    case 0x40: return 4;
    case 0x80:
    case 0x81:
    case 0x82: return 5;
    default:   return 0;
    }
}

/* The class of the object with this handle (0x5AF4): mcan_kind_class(mcan_object_kind()) */
u16 mcan_object_class(u16 handle)
{
    u16 kind = mcan_object_kind(handle);
    return mcan_kind_class((u8)kind);
}

/* The change counter of the object with this handle (0x5AFA): 1 for the master's objects,
 * mcan_sd_change_count for the slave's device, a slot's change_count; 0 none */
u16 mcan_object_change_count(u16 handle)
{
    u16 slot;
    lok_record_t *entry;
    lok_state_t *state;

    if (handle <= 2) return 1;
    if (handle == 0x100) return 1;
    if (handle == mcan_peer_handle) return mcan_sd_change_count;

    slot = handle - 0x200;
    if (slot > 9) return 0;

    entry = &lok_records[slot];
    if (entry->unused != 0) return 0;

    state = &lok_states[slot];
    return state->change_count;
}

/* The device controlling the object with this handle (0x5B42): 0x100 for the master's
 * objects, a slot's controller (an SD handle; 0 none) */
u16 mcan_object_controller(u16 handle)
{
    u16 slot;
    lok_record_t *entry;
    lok_state_t *state;

    if (handle <= 2) return 0x100;
    if (handle == 0x100) return 0;
    if (handle == mcan_peer_handle) return 0;

    slot = handle - 0x200;
    if (slot > 9) return 0;

    entry = &lok_records[slot];
    if (entry->unused != 0) return 0;

    state = &lok_states[slot];
    return state->controller;
}

/* Clear a slot's state, keeping its change counter, and give its functions their CAN objects
 * and keys again (0x5D76) */
void lok_state_clear(u16 slot)
{
    lok_state_t *state;
    u16 save_val;
    u8 *start, *end;

    state = &lok_states[slot];
    save_val = state->change_count;

    start = (u8 *)state;
    end = (u8 *)(state + 1);
    while (start < end) {
        *start++ = 0;
    }

    state->change_count = save_val;

    lok_assign_functions(slot);
}

/* The next object id after id in lok_id_table (step >= 0; before it for step < 0), going
 * round from the end to the table's first id: the first one of kind (mcan_object_kind),
 * of any kind for kind 0, or matching the second or third kind of the slot id names. The table's
 * first id is returned when it is reached; 0 when id is not in the table (and not 0). */
u16 mcan_next_object(u16 id, u8 kind, s8 step)
{
    s16 i, j;
    u8 k;
    const lok_record_t *e;

    i = 0;
    for (j = 0; j <= 14; j++) {
        if (lok_id_table[j] == id) {
            i = j;
            break;
        }
    }
    if (i == 0 && id != 0)
        return 0;
    for (;;) {
        if (step < 0) {
            i--;
        } else if (++i > 14) {
            i = 0;
        }
        k = (u8)mcan_object_kind(lok_id_table[i]);
        if (i == 0)
            break;
        if (k == 0)
            continue;
        if (k == kind || kind == 0)
            break;
        if ((u16)(id - 0x200) > 9)
            continue;
        e = &lok_records[id - 0x200];
        if (k == e->kinds[1] || k == e->kinds[2])
            break;
    }
    return lok_id_table[i];
}

/* The function of record e whose CAN object is object (0-15), 16 if none or object is 0
 * (0x713B). */
u16 lok_function_of_object(const lok_record_t *e, u8 object)
{
    const u8 *ptr;
    u16 idx;

    if (object == 0) return 16;

    ptr = &e->functions[0].object;
    for (idx = 0; idx <= 15; idx++) {
        if (*ptr == object) return idx;
        ptr += sizeof(lok_function_t);
    }
    return 16;
}

/* The highest CAN object number of record e: its speed object or a function's (0x7160). */
u16 lok_highest_object(const lok_record_t *e)
{
    u16 max_val = (u16)e->speed_object;
    const u8 *ptr = &e->functions[0].object;
    u16 count;
    u16 val;

    for (count = 16; count > 0; count--) {
        val = (u16)*ptr;
        ptr += sizeof(lok_function_t);
        if (val > max_val) {
            max_val = val;
        }
    }
    return max_val;
}

/*--------------------------------------------------------------------------
 * lok_find_free_slot - Find active slot with flags clear
 * 0x7185
 *
 * The first free slot (unused nonzero) that is not being removed: bit 12 of its state's
 * can_pending, save_pending and track_pending clear.
 *
 * Returns: slot index (0-9) or 0x0A (none found)
 *------------------------------------------------------------------------*/
s16 lok_find_free_slot(void)
{
    u16 slot;
    lok_state_t *disp = &lok_states[0];
    lok_record_t *r = &lok_records[0];
    u8 sync_flags;

    for (slot = 0; slot <= 9; slot++) {
        sync_flags = r->unused;
        r++;

        if (sync_flags == 0) goto next;

        if (HI8(disp->can_pending) & 0x10) goto next;
        if (HI8(disp->save_pending) & 0x10) goto next;
        if (HI8(disp->track_pending) & 0x10) goto next;

        return slot;
next:
        disp++;
    }
    return 0x0A;
}


/*--------------------------------------------------------------------------
 * lok_take_current - the current slot is driven here
 * 0xD898
 *
 * The current slot is driven here (LOKS_DRIVEN): announced on CAN, saved.
 *------------------------------------------------------------------------*/
void lok_take_current(void)
{
    lok_state_t *disp;
    s8 slot = (s8)lok_current_slot;
    disp = &lok_states[(s16)slot];
    disp->flags |= LOKS_DRIVEN;
    HI8(disp->can_pending) |= 0x04;
    HI8(disp->save_pending) |= 0x04;
}

/*--------------------------------------------------------------------------
 * lok_release_current - the current slot is no longer driven here
 * 0xD8B6
 *------------------------------------------------------------------------*/
void lok_release_current(void)
{
    lok_state_t *disp;
    s8 slot = (s8)lok_current_slot;
    disp = &lok_states[(s16)slot];
    disp->flags &= ~LOKS_DRIVEN;
    HI8(disp->can_pending) |= 0x04;
}

/*==========================================================================
 * Search & Matching Functions
 *========================================================================*/

/* The highest slot other than skip, in use and without an address conflict (flags bit 0),
 * with protocol proto and an address in addr..addr+n-1; an MM slot with a second address
 * (LOKP_F5_F8) also matches on that one. -1 if none. */
s16 lok_find_address(u16 addr, u8 proto, s8 skip, s8 n)
{
    s16 slot, j;
    const lok_record_t *e = &lok_records[LOK_SLOTS - 1];
    u16 a;

    for (slot = LOK_SLOTS - 1; slot != -1; slot--, e--) {
        for (j = 0; j < n; j++) {
            if (e->unused != 0 || proto != e->protocol || (e->flags & 1))
                continue;
            a = e->address;
            if (addr + j == a
                || (addr + j == a + 1 && proto == LOK_MM && (e->protocol_flags & LOKP_F5_F8))) {
                if (skip != slot)
                    return slot;
            }
        }
    }
    return slot;
}

/*--------------------------------------------------------------------------
 * lok_next_used_slot - Next slot in use (0xD963)
 *
 * From slot (a negative one counts as 0), steps by step through the slots 0-9, wrapping
 * around, to the first in use (unused 0); with step 0 the slot itself is
 * tried first, then the following ones. Returns the slot, or -1 when none is in use.
 *------------------------------------------------------------------------*/
s16 lok_next_used_slot(s8 slot, s8 step)
{
    s8 cur;

    if (slot < 0)
        slot = 0;
    if (step == 0) {
        slot = slot == 0 ? 9 : slot - 1;
        step = 1;
    }
    cur = slot;
    for (;;) {
        cur += step;
        if (cur < 0)
            cur = 9;
        if (cur > 9)
            cur = 0;
        if (lok_records[cur].unused == 0)
            return cur;
        if (cur == slot)
            return -1;
    }
}

/* lokstack_search_engine (0xD969), lokstack_search_return (0xD9B6),
 * lokstack_search_clamp (0xD9BB), lokstack_search_init (0xD9C3)
 * are internal entry points of lok_next_used_slot - not separate functions */

/* Clear the conflict flag of the first slot in use whose address is no longer used by
 * another slot, and mark its state for sending and saving. */
void lok_recheck_conflicts(void)
{
    u16 i;
    lok_record_t *e = lok_records;
    lok_state_t *d = lok_states;

    for (i = 0; (s16)i <= 9; i++, e++, d++) {
        if (e->unused != 0 || !(e->flags & 1))
            continue;
        if (lok_find_address(e->address, e->protocol, (s8)i,
                                  (e->protocol == LOK_MM && (e->protocol_flags & LOKP_F5_F8)) ? 2 : 1) < 0) {
            e->flags &= ~1;
            d->flags &= ~0x40;
            HI8(d->can_pending) |= 0x20;
            HI8(d->save_pending) |= 0x20;
            return;
        }
    }
}

/* Make slot's name unique among the slots in use: on a clash it becomes "name [n]" with the
 * next n (from 2), compared again from the first slot. */
void lok_make_name_unique(u16 slot)
{
    u8 buf[21];
    u16 n = 1;
    lok_record_t *e = &lok_records[slot];
    const lok_record_t *p;
    s16 i;

    str_format(buf, (const u8 *)"%s", e->name);
restart:
    for (i = 0, p = lok_records; i <= 9; i++, p++) {
        if (p->unused == 0 && i != (s16)slot && str_cmp(buf, p->name) == 0) {
            str_format(buf, (const u8 *)"%s [%d]", e->name, ++n);
            goto restart;
        }
    }
    str_ncpy(buf, e->name, 16);
}

extern void show_message(u8 countdown, u8 mode);  /* 0xDCA9 */

/* If another slot uses slot's address: show the conflict for 3 s (show_message), flag
 * it (flags bit 0) and mark its state for saving and sending. */
void lok_check_conflict(u16 slot)
{
    lok_record_t *e = &lok_records[slot];
    lok_state_t *d;

    if (lok_find_address(e->address, e->protocol, (s8)slot,
                              (e->protocol == LOK_MM && (e->protocol_flags & LOKP_F5_F8)) ? 2 : 1) < 0)
        return;
    show_message(3, 0x3E);
    e->flags |= 1;
    d = &lok_states[slot];
    HI8(d->save_pending) |= 0x20;
    HI8(d->can_pending) |= 0x20;
}

