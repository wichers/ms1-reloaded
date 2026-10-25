/*==========================================================================
 * ui/lok_icons.c - the locomotive's function icons and symbol on the display
 *
 * Functions:
 *   lcd_lok_icons          (0xACF1) - mode checks, then lcd_lok_icon_segments
 *   lcd_lok_icon_segments  (0xAD48) - the icon segments of the functions, the symbol
 *
 * The two share the 0x1E-byte stack frame lcd_lok_icons sets up; the original's code
 * of lcd_lok_icon_segments runs on to 0xAF4D.
 *
 * Segment table at 0xEE5F: 16-byte groups (4 entries of 4 bytes each).
 * Each entry: 2 bytes = (buf_offset_from_0x1108, mask).
 * Two entries per pair, two pairs per sub-slot.
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/lokstack.h"
#include "../include/rom_tables.h"
#include "../include/ui.h"


/* lib/runtime.c */
extern u16 compiler_shift_left_d_by_x(u16 value, u16 count);  /* 0xE109 */

/* Segment table base: 0xEE5F in ROM.
 * 16 groups, each 16 bytes = 4 sub-entries of 4 bytes.
 * Sub-entry = 2 pairs: (offset, mask, offset, mask). */
#define SEG_TABLE_BASE  ((u16)&lcd_blank_image[0x58])   /* lcd_icon_segments; this form compiles to the original code */
#define DISPLAY_BUF     ((u16)lcd_buf)  /* 0x1108 */

/*--------------------------------------------------------------------------
 * lcd_lok_icons - the icons of the selected locomotive (0xACF1)
 *
 * MENU (lcd_seg_menu_dcc bit 0): off in mode 1, blinking in mode 2 (on while ms_tick bits
 * 8-9 are not both 0), on otherwise; DCC (bit 4) for a DCC locomotive; ICON_SEIN
 * (lcd_seg_sein bit 4) from the slot state's flags bit 0. Then lcd_lok_icon_segments,
 * which shares its 0x1E-byte stack frame.
 *------------------------------------------------------------------------*/
void lcd_lok_icons(void)
{
    /* 0x3dacf1  leas -$1E,sp - frame shared with lcd_lok_icon_segments */

    u8 mode = operating_mode;

    /* 3dacf4-3dacff  operating_mode dispatch for lcd_seg_menu_dcc bit 0 */
    s16 slot;
    u8 blink;
    u8 decoder;
    u8 rflags;

    if (mode == 1) {
        /* 3db038  Mode 1: clear bit 0 */
        lcd_seg_menu_dcc &= ~0x01;
    } else if (mode == 2) {
        /* 3db02e  Mode 2: blink */
        blink = (u8)(ms_tick >> 8);
        if ((blink & 0x03) != 0) {
            /* 3dad03  blink phase on: set bit 0 */
            lcd_seg_menu_dcc |= 0x01;
        } else {
            /* 3db038  blink phase off: clear bit 0 */
            lcd_seg_menu_dcc &= ~0x01;
        }
    } else {
        /* 3dad03  Default: set bit 0 */
        lcd_seg_menu_dcc |= 0x01;
    }

    /* 3dad0b-3dad1e  the current slot's protocol (DCC icon) */
    slot = (s8)lok_current_slot;
    decoder = lok_records[slot].protocol;

    /* 3dad1e-3dad29  Set/clear lcd_seg_menu_dcc bit 4 */
    if (decoder == LOK_DCC) {
        /* 3db026  DCC: set bit 4 */
        lcd_seg_menu_dcc |= 0x10;
    } else {
        /* 3dad24  Clear bit 4 */
        lcd_seg_menu_dcc &= ~0x10;
    }

    /* 3dad2c-3dad42  the state's flags bit 0 */
    rflags = lok_states[slot].flags;

    if (rflags & 0x01) {
        /* 3dad42  Bit 0 set: set lcd_seg_sein bit 4 (via lcd_lok_icon_segments entry) */
        lcd_seg_sein |= 0x10;
    } else {
        /* 3db01e  Bit 0 clear: clear lcd_seg_sein bit 4 */
        lcd_seg_sein &= ~0x10;
    }

    /* Both paths enter lcd_lok_icon_segments */
    lcd_lok_icon_segments();
}

/*--------------------------------------------------------------------------
 * lcd_lok_icon_segments - the function icon segments
 * 0xAD48
 *
 * Uses the 0x1E-byte frame from lcd_lok_icons.
 * Updates the lcd_buf segments of the function key icons.
 *
 * Two nested loops:
 * 1. First loop (16 iterations): for each lokstack function group,
 *    SET segments for the active function slot, CLEAR other slots
 *    within the same group. Tracks processed groups in a bitmask.
 * 2. Second loop (9 iterations): CLEAR all segments for any group
 *    not visited by the first loop.
 *
 * Then the locomotive symbol (lok_record_t.symbol 1/3/2: lcd_seg_electric bit 3,
 * lcd_seg_steam bit 7, lcd_seg_diesel bit 3), blinking in operating_mode 2.
 * Before the loops: the "controller not working" icon (lcd_seg_controller bit 4), blinking
 * while another device controls the slot, steady with the state's flags bit 2.
 *
 * Segment table layout at 0xEE5F:
 *   16 groups x 16 bytes = 256 bytes total
 *   Each group: 4 sub-entries x 4 bytes
 *   Each sub-entry: pair0 (offset, mask) + pair1 (offset, mask)
 *------------------------------------------------------------------------*/
void lcd_lok_icon_segments(void)
{
    /* 3dad48-3dad4d  lcd_seg_sein already set/cleared by dispatcher */
    /* (The ASM entry does orab #$10; stab - but dispatcher handled it) */

    /* 3dad50-3dad5f  Save slot */
    s16 slot = (s8)lok_current_slot;

    /* 3dad62-3dad6f  the device driving the locomotive */
    u16 controller = lok_states[slot].controller;

    /* 3dad73-3dad7a  Address check */
    if (controller == 0 || controller == mcan_sd_handle) {
        /* 3daffb  Skip to alternate path: re-check flags bit 2 */
        u8 rflags2 = lok_states[slot].flags;
        if (rflags2 & 0x04) {
            /* 3daff3  Bit 2 set: lcd_seg_controller |= 0x10 */
            lcd_seg_controller |= 0x10;
        } else {
            /* 3db01b → 3dad88  Bit 2 clear: lcd_seg_controller &= ~0x10 */
            lcd_seg_controller &= ~0x10;
        }
        goto first_loop;
    }

    /* 3dad7e-3dad8d  blink */
    {
        u8 blink = (u8)(ms_tick >> 8);
        if ((blink & 0x03) != 0) {
            /* 3daff3  blink phase on: set lcd_seg_controller bit 4 */
            lcd_seg_controller |= 0x10;
        } else {
            /* 3dad88  blink phase off: clear lcd_seg_controller bit 4 */
            lcd_seg_controller &= ~0x10;
        }
    }

first_loop:
    /* ===== FIRST LOOP: Process 16 lokstack function groups ===== */
    /* 3dad90-3dae6a */
    {
        u16 counter = 0;           /* sp[0-1] */
        u16 processed_mask = 0;    /* arg_0 */
        u16 outer_count = 16;      /* arg_16 */

        do {
            /* 3dadaa-3dadb5  function counter */
            lok_function_t *fptr = &lok_records[slot].functions[counter];
            u8 func_active;
            u8 func_byte = fptr->key; /* arg_3 */
            u16 inner_idx;
            u8 high_nib;
            u8 low_nib;
            u16 inner_count;

            func_active = fptr->icon;

            if (func_active == 0) {
                /* 3dadc1  Inactive: skip to counter increment */
                goto next_outer;
            }

            /* 3dadc5-3dadcf  Extract nibbles */
            inner_idx = 0;     /* arg_4 */
            high_nib = func_byte >> 4; /* arg_D: group index */
            low_nib = func_byte & 0x0F; /* function slot within group */

            /* 3dadd5  Inner loop: 4 segment sub-entries per group */
            inner_count = 4;   /* arg_14 */

            do {
                /* 3daddb-3dade9  Calculate segment table pointer */
                u16 seg_offset = ((u16)high_nib * 4 + inner_idx) * 4;
                u8 *seg_ptr = (u8 *)(SEG_TABLE_BASE + seg_offset);

                /* 3dadeb  Read first entry pair */
                u16 entry_word = *(u16 *)seg_ptr;
                if (entry_word == 0) {
                    /* Empty entry: skip to counter */
                    goto next_inner;
                }

                /* 3dadf4  Check if this is the function's own slot */
                if (low_nib == (u16)(inner_idx & 0xFFFF)) {
                    /* === SET PATH (0xAF99) === */
                    /* 3daf99  OR mask into display buffer (first pair) */
                    u8 buf_off1 = (u8)(entry_word >> 8);
                    u8 mask1 = seg_ptr[1];
                    u16 func_bits;
                    u16 func_mask;

                    *(u8 *)(DISPLAY_BUF + buf_off1) |= mask1;

                    /* 3dafae-3dafd3  Check func_bits for this function */
                    func_bits = lok_states[slot].functions;
                    func_mask = compiler_shift_left_d_by_x(1, counter);
                    if (func_bits & func_mask) {
                        /* 3dafd6  Function on: also OR the second pair (the "on" variant) */
                        u16 entry2 = *(u16 *)(seg_ptr + 2);
                        if (entry2 != 0) {
                            u8 buf_off2 = (u8)(entry2 >> 8);
                            u8 mask2 = seg_ptr[3];
                            *(u8 *)(DISPLAY_BUF + buf_off2) |= mask2;
                        }
                    } else {
                        /* 3daff0 -> 3dae21  Function off: clear the second pair */
                        u16 entry2 = *(u16 *)(seg_ptr + 2);
                        if (entry2 != 0) {
                            u8 buf_off2 = (u8)(entry2 >> 8);
                            u8 mask2 = seg_ptr[3];
                            *(u8 *)(DISPLAY_BUF + buf_off2) &= ~mask2;
                        }
                    }
                    /* Skip to next inner (3dae39 via 3dae37/3daff0) */
                    goto next_inner;
                }

                /* 3dadfa-3dae06  Check if group already processed */
                {
                    u16 grp_mask = compiler_shift_left_d_by_x(1, (u16)high_nib);
                    if (processed_mask & grp_mask) {
                        /* Already processed: skip */
                        goto next_inner;
                    }
                }

                /* === CLEAR PATH (0xAE09) === */
                /* 3dae09-3dae1f  Clear first pair from display buffer */
                {
                    u16 e1 = *(u16 *)seg_ptr; /* re-read (X clobbered) */
                    if (e1 != 0) {
                        u8 off1 = (u8)(e1 >> 8);
                        u8 m1 = seg_ptr[1];
                        *(u8 *)(DISPLAY_BUF + off1) &= ~m1;
                    }
                }

                /* 3dae21-3dae37  Clear second pair */
                {
                    u16 e2 = *(u16 *)(seg_ptr + 2);
                    if (e2 != 0) {
                        u8 off2 = (u8)(e2 >> 8);
                        u8 m2 = seg_ptr[3];
                        *(u8 *)(DISPLAY_BUF + off2) &= ~m2;
                    }
                }

next_inner:
                /* 3dae39-3dae45  Inner counter */
                inner_idx++;
                inner_count--;
            } while (inner_count != 0);

            /* 3dae47-3dae5a  Mark group as processed */
            {
                u16 nib_mask = compiler_shift_left_d_by_x(1, (u16)(high_nib & 0x0F));
                processed_mask |= nib_mask;
            }

next_outer:
            /* 3dae5c-3dae6a  Outer counter */
            counter++;
            outer_count--;
        } while (outer_count != 0);

        /* ===== SECOND LOOP: Clear unprocessed groups ===== */
        /* 3dae6e-3daee8 */
        {
            u16 counter2 = 0;
            u16 seg_group_idx = 0; /* arg_E: index into segment table, stride 4 */
            u16 loop_count2 = 9;   /* arg_12 */

            do {
                /* 3dae7c-3dae88  Check if group was processed */
                u16 grp_mask2 = compiler_shift_left_d_by_x(1, counter2);
                u16 tbl_off;
                u16 sub_count;

                if (processed_mask & grp_mask2) {
                    /* Already processed: skip */
                    goto next_outer2;
                }

                /* 3dae8b-3daed3  Clear all 4 sub-entries for this group */
                tbl_off = seg_group_idx * 4;
                sub_count = 4; /* arg_10 */

                do {
                    /* 3dae98  First pair of sub-entry */
                    u8 *sp1 = (u8 *)(SEG_TABLE_BASE + tbl_off);
                    u16 w1 = *(u16 *)sp1;
                    u16 w2;
                    if (w1 != 0) {
                        u8 o1 = (u8)(w1 >> 8);
                        u8 mk1 = sp1[1];
                        *(u8 *)(DISPLAY_BUF + o1) &= ~mk1;
                    }

                    /* 3daeb0  Second pair of sub-entry */
                    w2 = *(u16 *)(sp1 + 2);
                    if (w2 != 0) {
                        u8 o2 = (u8)(w2 >> 8);
                        u8 mk2 = sp1[3];
                        *(u8 *)(DISPLAY_BUF + o2) &= ~mk2;
                    }

                    /* 3daec8  Advance by 4 bytes */
                    tbl_off += 4;
                    sub_count--;
                } while (sub_count != 0);

next_outer2:
                /* 3daed5-3daee8  Outer counter */
                counter2++;
                seg_group_idx += 4; /* stride 4 sub-entries per group */
                loop_count2--;
            } while (loop_count2 != 0);
        }
    }

    /* ===== the locomotive symbol (0xAEED-0xAF4D) ===== */
    /* The symbol of the current slot: its icon on, blinking in mode 2.
     * For each type (1, 3, 2): if match AND (mode != 2 or the blink phase is on)
     * → SET flag bit; else → CLEAR flag bit.
     */
    {
        u8 symbol;

        /* 3daefc  Check symbol vs 1 → lcd_seg_electric bit 3 */
        symbol = lok_records[slot].symbol;
        if (symbol == 1) {
            /* 3daf7e  Type 1 match: check mode/blink */
            if (operating_mode != 2 || ((ms_tick >> 8) & 0x03)) {
                lcd_seg_electric |= 0x08;
            } else {
                lcd_seg_electric &= ~0x08;
            }
        } else {
            /* 3daf04  Type 1 mismatch: clear bit */
            lcd_seg_electric &= ~0x08;
        }

        /* 3daf1b  Check symbol vs 3 → lcd_seg_steam bit 7 */
        symbol = lok_records[slot].symbol;
        if (symbol == 3) {
            /* 3daf66  Type 3 match: check mode/blink */
            if (operating_mode != 2 || ((ms_tick >> 8) & 0x03)) {
                lcd_seg_steam |= 0x80;
            } else {
                lcd_seg_steam &= ~0x80;
            }
        } else {
            /* 3daf23  Type 3 mismatch: clear bit */
            lcd_seg_steam &= ~0x80;
        }

        /* 3daf3a  Check symbol vs 2 → lcd_seg_diesel bit 3 */
        symbol = lok_records[slot].symbol;
        if (symbol == 2) {
            if (operating_mode != 2 || ((ms_tick >> 8) & 0x03)) {
                lcd_seg_diesel |= 0x08;
            } else {
                lcd_seg_diesel &= ~0x08;
            }
        } else {
            /* 3daf42  Type 2 mismatch: clear bit */
            lcd_seg_diesel &= ~0x08;
        }
    }

    /* 3daf4a  leas $1E,sp; rts - handled by C return */
}

