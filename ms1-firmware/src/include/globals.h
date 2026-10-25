/*==========================================================================
 * globals.h - the firmware's RAM variables
 *
 * Each at the original's address (the comments), defined in globals.c. 0x1000-0x102F is the
 * original's .data (initial values copied by crt_restart), the rest its .bss.
 * docs/architecture.md describes how the subsystems use them.
 *========================================================================*/
#ifndef GLOBALS_H
#define GLOBALS_H

#include "hcs12.h"
#include "lokstack.h"

/*--- System ---*/
extern u16 ms_tick;                         /* 0x13AC: main loop passes, one per ms (wait_for_next_ms_tick) */
extern u8  is_master;                       /* 0x13A8: the central (track voltage at start-up) */
extern u8  system_status_flags;             /* 0x13A9: bit 0 STOP, 1 overload, 2 mfx registration,
                                             * 3 MM programming, 5, 7 message shown */
extern u8  operating_mode;                  /* 0x13B5: 0 start-up, 1 driving, 2 slot choice, 3 menus, 4 records */
extern u8  lok_current_slot;                /* 0x13B4: the selected slot, -1 none */
extern u8  message_ticks;                   /* 0x13AA: UI passes left (show_message) */
extern u8  message_id;                      /* 0x13AB: the message, LANG(0x16 + message_id) */
extern u8  language_index;                  /* 0x1030 */
extern u8  device_name[16];                 /* 0x190F: this device's name, 0-terminated */
extern u16 can_device_uid;                  /* 0x13C3: (flash_hw_id ^ mfx_reg_counter) | 1, written at
                                             * start-up and never read */
extern char end_of_data_marker[13];         /* 0x1023: "End of .data", the end of the original's .data */

/*--- Clock (isr_ms_tick) ---*/
extern volatile u16 rtc_ms_counter;         /* 0x1BB5: ms */
extern volatile u16 rtc_minute_ms;          /* 0x1BB3: ms within the minute */
extern volatile u16 rtc_day_minutes;        /* 0x1BB1: minutes within the day */
extern volatile u16 rtc_day_counter;        /* 0x1BAF: days */

/*--- Track signal: the command buffer (track_cmd.h) and the generator (build/asm/siggen.s) ---*/
extern u8  track_cmd_buf[48];               /* 0x1268: 4 entries of 12 bytes */
extern u16 track_cmd_read_ptr;              /* 0x101C: the entry the generator works on */
extern u16 track_cmd_write_ptr;             /* 0x101E: the entry being written */
extern u8  track_cmd_free;                  /* 0x1020: free entries, 0-4 */
extern u8  track_cmd_pos;                   /* 0x1298: step within the current entry */
extern u8  signal_gen_prev_cmd_type;        /* 0x129B */
extern u8  signal_gen_send_buffer[64];      /* 0x1228: (code, argument) byte pairs from
                                             * signal_gen_cmd_buf_consumer for signal_gen_send_buf_executor */
extern u16 signal_gen_send_buf_read_ptr;    /* 0x1018 */
extern u16 signal_gen_send_buf_write_ptr;   /* 0x101A */
extern u8  signal_staging_buf[200];         /* 0x1160: timing entries for the timer channel 4 interrupt */
extern volatile u16 signal_gen_staging_read_ptr;   /* 0x1014 */
extern volatile u16 signal_gen_staging_write_ptr;  /* 0x1016 */
extern u16 signal_timer_compare_base;       /* 0x1021: timer ticks of the next edge */
extern u8  track_polarity;                  /* 0x12A0: output level of the next edge, 0x10 / 0x20 */
extern u8  mfx_stuff_count;                 /* 0x12A1: mfx ones left before a stuffed 0 */
extern u8  mfx_bit_pos;                     /* 0x12A2: mfx frame being built: bit, */
extern u8  mfx_byte_idx;                    /* 0x12A3: byte, */
extern u8  mfx_crc_state;                   /* 0x12A4: CRC */

/*--- Track signal: what goes out (track_scheduler, lok_packets_next, track_filler) ---*/
extern u8  pkt_slot;                        /* 0x1031: slot served by lok_packets_next */
extern u8  pkt_steps;                       /* 0x1032: packets left of its job */
extern u8  pkt_kind;                        /* 0x1033: the job (signal/lok_packets.c) */
extern u8  mm_prog_addr;                    /* 0x1034: MM programming: the address of the packets */
extern u8  refresh_slot;                    /* 0x1035: slot of the next periodic packet */
extern u8  track_filler_state;              /* 0x105A: track_filler's step, 0-5 */
extern volatile u8  track_filler_hold;      /* 0x105B: never written: always 0 */
extern volatile u8  track_scheduler_hold;   /* 0x1036: never set: always 0; holds back
                                             * lok_packets_next and the mfx registration */

/*--- Track monitor: current, overload, faults (track_current_monitor) ---*/
extern u16 track_current;                   /* 0x10F2: mA */
extern u16 track_current_offset;            /* 0x10F4: current sense zero, mA */
extern u16 overload_slow;                   /* 0x10F6: overload accumulators */
extern u16 overload_fast;                   /* 0x10F8 */
extern u8  shutdown_count;                  /* 0x10FA: overload shutdowns since shutdown_ticks reached 1000 */
extern u16 shutdown_ticks;                  /* 0x10FB: ms since the last overload shutdown */
extern u8  track_fault_prev;                /* 0x100A: system_status_flags & 3 of the last pass */
extern u16 track_fault_timer;               /* 0x10AE: overload/voltage fault timer; in start-up mode the
                                             * passes STOP and the knob are held (factory reset above 0x1E) */
extern u16 minute_countdown;                /* 0x100B: ms to the next operating minute */
extern u16 adc_track_voltage;               /* 0x13A4: mV */
extern u16 adc_decoder_voltage;             /* 0x13A6: mV */

/*--- mfx: feedback capture (build/asm/hal.s, signal/mfx_feedback.c) ---*/
extern u8  track_capture_cmd;               /* 0x129C: capture command at the next marked staging entry:
                                             * 1 start bit mode, 2 start ATD mode, 0x81/0x82 stop */
extern u8  track_capture_next;              /* 0x129D: the command queued after it */
extern u16 track_capture_samples;           /* 0x129E: samples the last stop returned; 0 none */
extern u8  timer_capture_buffer[96];        /* 0x1B4B: bit mode: two bits (PT1, PB5) per edge, 8 per
                                             * word; ATD mode: one ATD0 channel 2 byte per edge */
extern volatile u16 timer_capture_ptr;      /* 0x1BAB */
extern volatile u8  timer_capture_count;    /* 0x1BAD */
extern volatile u8  timer_capture_mode;     /* 0x1BAE: mode of the running capture */
extern u16 mfx_decode_read_ptr;             /* 0x13A0: mfx_feedback_read_bit's position */
extern u8  mfx_decode_bits_remaining;       /* 0x13A2 */
extern u8  mfx_decode_current_byte;         /* 0x13A3 */

/*--- mfx: registration and read-out (signal/mfx_register.c) ---*/
extern u16 mfx_next_address;                /* 0x1000: the next new track address */
extern u16 mfx_reg_counter;                 /* 0x13C1: the central's registration counter */
extern u8  mfx_retry;                       /* 0x1037 */
extern u8  mfx_slot;                        /* 0x1038 */
extern u8  mfx_state;                       /* 0x1039 */
extern u8  mfx_substate;                    /* 0x103A */
extern u8  mfx_block;                       /* 0x103B */
extern volatile u8  mfx_cmd_steps;          /* 0x103C: steps left of the current command: 3 frame,
                                             * 2 syncs, 1 feedback window */
extern u8  mfx_read_offset;                 /* 0x103D */
extern u8  mfx_read_len;                    /* 0x103E */
extern u8  mfx_read_buf[2];                 /* 0x103F */
extern u8  mfx_block02_groups;              /* 0x1041 */
extern u8  mfx_block01_cas;                 /* 0x1042 */
extern u16 mfx_cv;                          /* 0x1043 */
extern u16 mfx_uid_hi;                      /* 0x1045 */
extern u16 mfx_uid_lo;                      /* 0x1047 */
extern u16 mfx_ca_protocol;                 /* 0x1049 */
extern u16 mfx_ca_symbol;                   /* 0x104B */
extern u16 mfx_ca_blocks;                   /* 0x104D */
extern u16 mfx_ca_functions;                /* 0x104F */
extern u8  mfx_block02;                     /* 0x1051 */
extern u8  mfx_block04;                     /* 0x1052 */
extern u8  mfx_block05;                     /* 0x1053 */
extern u8  mfx_block08;                     /* 0x1054 */
extern u8  mfx_block02_cas;                 /* 0x1055 */
extern u8  mfx_block04_cas;                 /* 0x1056 */
extern u8  mfx_block05_cas;                 /* 0x1057 */
extern u8  mfx_block08_cas;                 /* 0x1058 */
extern u8  mfx_search_flag;                 /* 0x1059: the next search step counts as answered (set
                                             * by mfx_registration_start for the UID kept in EEPROM) */

/*--- CAN: driver rings (build/asm/hal.s), 13-byte MSCAN frame images ---*/
extern u8  can_tx_ring[130];                /* 0x1929: 10 frames */
extern u8  can_rx_ring[416];                /* 0x19AB: 32 frames */
extern volatile u16 can_rx_ring_read;       /* 0x1921 */
extern volatile u16 can_rx_ring_write;      /* 0x1923 */
extern volatile u16 can_tx_ring_read;       /* 0x1925 */
extern volatile u16 can_tx_ring_write;      /* 0x1927 */

/*--- CAN: M-CAN protocol (src/can/; the protocol document's terms) ---*/
extern u8  can_rx_frame_buf[13];            /* 0x106F: the frame being handled (mcan_receive) */
extern u16 mcan_rx_handle;                  /* 0x107C: its object handle, ID bits 10-25 */
extern u8  mcan_rx_node;                    /* 0x107E: its sender's node number, ID bits 0-6 */
extern u8  mcan_rx_cmd;                     /* 0x107F: its command, ID bits 7-9 */
extern u8  mcan_node;                       /* 0x13C7: this device's node number (slave; 0 master) */
extern u8  mcan_peer_node;                  /* 0x105C: the other side's node number */
extern u16 mcan_peer_handle;                /* 0x105D: handle of the object standing for the other device */
extern u16 mcan_peer_uid_hi;                /* 0x1063: the other device's UID */
extern u16 mcan_peer_uid_lo;                /* 0x1065 */
extern u16 mcan_peer_timeout;               /* 0x1069: passes until the other side counts as lost */
extern u16 mcan_sd_handle;                  /* 0x13C5: this device's own device-object (SD) handle */
extern u16 mcan_sd_change_count;            /* 0x105F: change counter of the slave's SD object */
extern u16 mcan_sy_handle;                  /* 0x1002: slave: the master's SY handle (status) */
extern u16 mcan_sy_change_count;            /* 0x1061: its change counter */
extern u16 mcan_start_handle;               /* 0x1004: slave: the start-object handle of its requests */
extern u8  mcan_reg_mid;                    /* 0x1067: registration MID (hash bits 0-4) */
extern u8  mcan_reg_step;                   /* 0x1068: registration and setup step */
extern u8  mcan_link_flags;                 /* 0x106D: link frames due: bit 0 the step's frame, 1 monitor,
                                             * 2 identification answer, 7 the bus-occupied burst sent */
extern u16 mcan_list_index;                 /* 0x106B: position in the list being queried */
extern u16 mcan_status_pending;             /* 0x13BF: bits 0-3 status changes to send (as
                                             * system_status_flags), 7 device name, 9 emergency stop */
extern u8  mcan_status_idx_overload;        /* 0x1006: the master's status-list index of each status, */
extern u8  mcan_status_idx_stop;            /* 0x1007: 0 none */
extern u8  mcan_status_idx_mfx_reg;         /* 0x1008 */
extern u8  mcan_status_idx_mm_prog;         /* 0x1009 */
extern u8  mcan_name_send_pos;              /* 0x106E: position + 1 of the next device_name part; 0 none */

/*--- User interface: menus ---*/
extern u8  menu_top;                        /* 0x13B6: menu level 1, 0 = not entered */
extern u8  menu_item;                       /* 0x13B7: level 2, the item of menu_top */
extern u8  menu_sub;                        /* 0x13B8: level 3 */
extern u8  menu_sub2;                       /* 0x13B9: level 4, 1 when entered */
extern u16 menu_value;                      /* 0x10B0: the value being edited */
extern u16 menu_value2;                     /* 0x10B2: a second one: database record count, the new
                                             * address, the CV value */
extern u16 menu_value3;                     /* 0x10B4: the chosen symbol; database: article number bit 16 */
extern u16 menu_value4;                     /* 0x10B6: database: article number, low 16 bits; address
                                             * entry: the protocol flags */
extern u16 menu_db_brand;                   /* 0x10B8: 'M' or 'T' (Trix) before the article number */
extern u8  lok_name_edit_buf[56];           /* 0x10BA: a database record (page $3C), the name being edited */
extern u8  ui_settings;                     /* 0x13BA: EEPROM 0x0801: bits 0-1 knob speed mode,
                                             * 2-3 text display (4 alternative glyphs, 8 scrolling),
                                             * 4 knob push drives */

/*--- User interface: keys and knob ---*/
extern u8  porta_buttons_raw;               /* 0x13AE */
extern u8  porta_buttons_edge;              /* 0x13AF */
extern u8  button_state_raw;                /* 0x13B0 */
extern u8  button_state_edge;               /* 0x13B1 */
extern u8  ui_esc_held;                     /* 0x13B2: 32 ms passes MENU/ESC (PB0) is held, saturating */
extern u8  ui_stop_held;                    /* 0x13B3: the same for STOP (PB3) */
extern u8  encoder_count;                   /* 0x1101: the knob value */
extern u8  encoder_mode;                    /* 0x1102: 0 clamps at the range, else wraps */
extern u16 encoder_range;                   /* 0x100D */
extern u8  encoder_step;                    /* 0x100F: added per transition (flash 0xFF64 when not 0) */
extern u16 encoder_debounce_mask;           /* 0x1010 */
extern u16 encoder_a_debounce;              /* 0x1103 */
extern u16 encoder_b_debounce;              /* 0x1105 */
extern u8  encoder_ab_state;                /* 0x1107: the last debounced A/B levels */
extern u16 encoder_last_position;           /* 0x191F */
extern u16 encoder_clicks_hi;               /* 0x10FD: knob transitions, a 32-bit count (record "CL") */
extern u16 encoder_clicks_lo;               /* 0x10FF */

/*--- Display ---*/
extern u8  lcd_buf[88];                     /* 0x1108: the PT6547 command stream, [strobe, data] pairs */
extern volatile u16 lcd_pump_ptr;           /* 0x1012: the SPI interrupt's position in lcd_buf */
extern u8  lcd_text[32];                    /* 0x108C: the text line (ui_screen_text) */
extern u16 lcd_text_offset;                 /* 0x10AC: its placement and scroll state (lcd_text.c) */

/*--- EEPROM (sys/eeprom_save.c) ---*/
extern u16 ee_save_pending;                 /* 0x13BD: items to save */
extern u8  ee_save_step;                    /* 0x1080: steps left of the current item */
extern u8  ee_save_item;                    /* 0x1081: item being saved, a slot 0-9 or 11-16 */
extern u16 ee_record_addr;                  /* 0x1082: record erased, to be programmed; 0 none */
extern u16 ee_record[2];                    /* 0x1084: its new content */
extern u16 ee_counter_interval;             /* 0x1088: passes between counter saves, grows */
extern u16 ee_counter_ticks;                /* 0x108A */
extern u8  ee_counter_record;               /* 0x13C8: eeprom_record entry saved next (0-2) */
extern u16 ee_counter_addr;                 /* 0x13D5: next record of the EEPROM ring (EE_RECORDS) */
extern u16 eeprom_record[3][2];             /* 0x13C9: newest EEPROM record of each kind (1-3) */

/* Start-up code (gcc/crt0.s) */
extern void crt_restart(void) __attribute__((noreturn));      /* 0x403C: restart the application */
extern void enter_bootloader(void) __attribute__((noreturn)); /* 0x407C: hand over to the bootloader */

/* Fields of other variables, named on their own by the code (the original has no separate
 * variable at these addresses). */
#define mcan_rx_data01          (*(u16 *)&can_rx_frame_buf[4])     /* data[0..1] */
#define mcan_rx_data23          (*(u16 *)&can_rx_frame_buf[6])     /* data[2..3] */
#define mcan_rx_data45          (*(u16 *)&can_rx_frame_buf[8])     /* data[4..5] */
#define mcan_rx_data67          (*(u16 *)&can_rx_frame_buf[10])    /* data[6..7] */
#define mcan_rx_dlc             (can_rx_frame_buf[12])
#define lokdb_rec_info          (lok_name_edit_buf[2])  /* bits 0-3 icons, 4-5 symbol, 6 Trix, 7 article bit 16 */
#define lokdb_rec_protocol      (lok_name_edit_buf[3])  /* bits 0-1 protocol - 2, 2-7 protocol flags */
#define lokdb_rec_address       (lok_name_edit_buf[4])  /* bits 0-6 address, 7 another locomotive follows */
#define lcd_seg_menu_dcc        (lcd_buf[0x11])         /* bit 0 MENU, 4 DCC */
#define lcd_seg_controller      (lcd_buf[0x13])         /* bit 4 another device controls the slot */
#define lcd_seg_sein            (lcd_buf[0x1B])         /* bit 4 ICON_SEIN, 5 right arrow */
#define lcd_seg_steam           (lcd_buf[0x33])
#define lcd_seg_diesel          (lcd_buf[0x3B])
#define lcd_seg_electric        (lcd_buf[0x49])
#define operating_minutes       ((u8 *)eeprom_record[0])   /* 24-bit counters */
#define overload_events         ((u8 *)eeprom_record[1])
#define encoder_clicks_rec_hi   (eeprom_record[2][0])
#define encoder_clicks_rec_lo   (*(u8 *)&eeprom_record[2][1])

#endif /* GLOBALS_H */
