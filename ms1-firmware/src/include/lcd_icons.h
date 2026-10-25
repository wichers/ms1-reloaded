#ifndef LCD_ICONS_H
#define LCD_ICONS_H

/*==========================================================================
 * lcd_icons.h - the icon segments of the LCD
 *
 * A-series: indices into lcd_icon_segments (0xEE5F), an (lcd_buf offset, mask) pair each.
 * B-series: indices into lcd_bar_segments (0xF450), the speed bar.
 * C-series: lcd_buf bits in no table, as (offset, mask) pairs.
 *========================================================================*/

/*--- Icon segment table parameters ---*/
#define ICON_SEG_COUNT              83

/*--- A-series: Function key icons (F1-F8) ---
 * Grouped in 16-byte blocks (8 entries per function key).
 * Each function key has ON/OFF pairs for 2-3 icon variants.
 * Unused entries within a group have mask=0x00. */

/* Group F1 (A0-A7) */
#define ICON_F1_LIGHT1_OFF          0       /* A0  */
#define ICON_F1_LIGHT1_ON           1       /* A1  */
#define ICON_F1_BELL_OFF            2       /* A2  */
#define ICON_F1_BELL_ON             3       /* A3  */
#define ICON_F1_TURTLE_OFF          4       /* A4  */
#define ICON_F1_TURTLE_ON           5       /* A5  */

/* Group F2 (A8-A15) */
#define ICON_F2_SPEAKER_OFF         8       /* A8  */
#define ICON_F2_SPEAKER_ON          9       /* A9  */
#define ICON_F2_RAMP_OFF            10      /* A10 */
#define ICON_F2_RAMP_ON             11      /* A11 */

/* Group F3 (A16-A23) */
#define ICON_F3_SPEAKER_OFF         16      /* A16 */
#define ICON_F3_SPEAKER_ON          17      /* A17 */
#define ICON_F3_TURTLE_OFF          18      /* A18 */
#define ICON_F3_TURTLE_ON           19      /* A19 */
#define ICON_F3_FUNCTION_OFF        20      /* A20 */
#define ICON_F3_FUNCTION_ON         21      /* A21 */

/* Group F4 (A24-A31) */
#define ICON_F4_MUSIC_OFF           24      /* A24 */
#define ICON_F4_MUSIC_ON            25      /* A25 */
#define ICON_F4_RAMP_OFF            26      /* A26 */
#define ICON_F4_RAMP_ON             27      /* A27 */
#define ICON_F4_LIGHT_OFF           28      /* A28 */
#define ICON_F4_LIGHT_ON            29      /* A29 */

/* Group F5 (A32-A39) */
#define ICON_F5_HORN_OFF            32      /* A32 */
#define ICON_F5_HORN_ON             33      /* A33 */
#define ICON_F5_LIGHT_OFF           34      /* A34 */
#define ICON_F5_LIGHT_ON            35      /* A35 */
#define ICON_F5_FUNCTION_OFF        36      /* A36 */
#define ICON_F5_FUNCTION_ON         37      /* A37 */

/* Group F6 (A40-A47) */
#define ICON_F6_HORN_OFF            40      /* A40 */
#define ICON_F6_HORN_ON             41      /* A41 */
#define ICON_F6_CONNECT_OFF         42      /* A42 */
#define ICON_F6_CONNECT_ON          43      /* A43 */

/* Group F7 (A48-A55) */
#define ICON_F7_STEAM_OFF           48      /* A48 */
#define ICON_F7_STEAM_ON            49      /* A49 */
#define ICON_F7_HOOK_OFF            50      /* A50 */
#define ICON_F7_HOOK_ON             51      /* A51 */
#define ICON_F7_BELL_OFF            52      /* A52 */
#define ICON_F7_BELL_ON             53      /* A53 */

/* Group F8 (A56-A63) */
#define ICON_F8_LIGHT2_OFF          56      /* A56 */
#define ICON_F8_LIGHT2_ON           57      /* A57 */
#define ICON_F8_LIGHT_OFF           58      /* A58 */
#define ICON_F8_LIGHT_ON            59      /* A59 */
#define ICON_F8_MUSIC_OFF           60      /* A60 */
#define ICON_F8_MUSIC_ON            61      /* A61 */

/*--- A-series: Status/mode icons (A64-A82) ---*/
#define ICON_LIGHT_OFF              64      /* A64 */
#define ICON_LIGHT_ON               65      /* A65 */

#define ICON_DCC                    72      /* A72 */
#define ICON_SEIN                   73      /* A73 */
#define ICON_CONTROLLER_NOT_WORKING 74      /* A74 */
#define ICON_TRAIN_STEAM            75      /* A75 */
#define ICON_TRAIN_ELECTRIC         76      /* A76 */
#define ICON_TRAIN_DIESEL           77      /* A77 */
#define ICON_MFX                    78      /* A78 */
#define ICON_MENU                   79      /* A79 */
#define ICON_SHORT_CIRCUIT          80      /* A80 */
#define ICON_WARNING                81      /* A81 */
#define ICON_STOP                   82      /* A82 */

/*--- Status icons the firmware switches directly: display buffer byte, segment mask ---*/
#define ICON_MFX_BYTE               0x0F
#define ICON_MFX_MASK               0x01
#define ICON_MENU_BYTE              0x11
#define ICON_MENU_MASK              0x01
#define ICON_TRAIN_STEAM_BYTE       0x33
#define ICON_TRAIN_STEAM_MASK       0x80
#define ICON_TRAIN_ELECTRIC_BYTE    0x49
#define ICON_TRAIN_ELECTRIC_MASK    0x08
#define ICON_TRAIN_DIESEL_BYTE      0x3B
#define ICON_TRAIN_DIESEL_MASK      0x08
#define ICON_SHORT_CIRCUIT_BYTE     0x49
#define ICON_SHORT_CIRCUIT_MASK     0x80
#define ICON_WARNING_BYTE           0x33
#define ICON_WARNING_MASK           0x08
#define ICON_STOP_BYTE              0x3B
#define ICON_STOP_MASK              0x80

/* icon_on(ICON_MFX), icon_off(ICON_MFX); the caller includes globals.h */
#define icon_on(i)   (lcd_buf[i##_BYTE] |= i##_MASK)
#define icon_off(i)  (lcd_buf[i##_BYTE] &= ~i##_MASK)

/* End argument of lcd_clear_segments: it clears the icons before index i */
#define ICONS_BEFORE(i)  (&lcd_icon_segments[2 * (i)])

/*--- B-series: Speed indicator bar segments (lcd_bar_segments, 14 entries) ---*/
#define SPEED_IND_B0                0
#define SPEED_IND_B1                1
#define SPEED_IND_B2                2
#define SPEED_IND_B3                3
#define SPEED_IND_B4                4
#define SPEED_IND_B5                5
#define SPEED_IND_B6                6
#define SPEED_IND_B7                7
#define SPEED_IND_B8                8
#define SPEED_IND_B9                9
#define SPEED_IND_B10               10
#define SPEED_IND_B11               11
#define SPEED_IND_B12               12
#define SPEED_IND_B13               13
#define SPEED_IND_COUNT             14

/*--- C-series: Unmapped display segments (not in ROM tables) ---
 * These segments were found by the walker as bits not covered by
 * any ROM table. Defined as direct (buffer_offset, mask) pairs.
 * Buffer offsets are relative to display buffer base at 0x1108. */
#define ICON_ARROW_LEFT_OFF         0x0B    /* C0 */
#define ICON_ARROW_LEFT_MASK        0x10    /* C0: bit 4 */
#define ICON_MARKINGS_OFF           0x0D    /* C1 */
#define ICON_MARKINGS_MASK          0x02    /* C1: bit 1 */
#define ICON_ARROW_RIGHT_OFF        0x1B    /* C6 */
#define ICON_ARROW_RIGHT_MASK       0x20    /* C6: bit 5 */

#endif /* LCD_ICONS_H */
