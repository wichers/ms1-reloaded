/*==========================================================================
 * globals.c - RAM global definitions
 *
 * Every variable sits at its address in the original firmware: RAM(addr) / RAM_DATA(addr)
 * put it in the section .bss.ram.<addr> / .data.ram.<addr> (RAM_DATA for the initialised
 * variables), and tools/ramlayout.py turns those sections into the linker script's
 * RAM layout (build/ms1.ld), so the original's RAM map - and its RAM contents, for the
 * differential tests - is reproduced. Variables at 0x1000-0x102F are the original's .data
 * (initial values copied from flash by the start-up code), the rest is .bss up to the GCC
 * soft registers at 0x1BB7.
 *========================================================================*/

#include "include/hcs12.h"
#include "include/lokstack.h"

/* .bss.* and .data.* names give the sections the types GCC gives .bss and .data */
#define RAM(addr)      __attribute__((section(".bss.ram." #addr)))
#define RAM_DATA(addr) __attribute__((section(".data.ram." #addr)))

/* The original's .data, 0x1000-0x102F: the initial values are the original's */
/* (the buffers the pointers below start at, defined first for their initialisers) */
RAM(0x1108) u8 lcd_buf[88];
RAM(0x1160) u8 signal_staging_buf[200];
RAM(0x1228) u8 signal_gen_send_buffer[64];
RAM(0x1268) u8 track_cmd_buf[48];
RAM_DATA(0x1000) u16 mfx_next_address = 0x36AF;  /* the address a new mfx locomotive gets, counting down */
RAM_DATA(0x1002) u16 mcan_sy_handle = 0x0002;
RAM_DATA(0x1004) u16 mcan_start_handle = 0x0001;
RAM_DATA(0x1006) u8 mcan_status_idx_overload = 0x02;
RAM_DATA(0x1007) u8 mcan_status_idx_stop = 0x01;
RAM_DATA(0x1008) u8 mcan_status_idx_mfx_reg = 0x04;
RAM_DATA(0x1009) u8 mcan_status_idx_mm_prog = 0x03;
RAM_DATA(0x100A) u8 track_fault_prev = 0x01;
RAM_DATA(0x100B) u16 minute_countdown = 0xEA60;
RAM_DATA(0x100D) u16 encoder_range = 0x0100;
RAM_DATA(0x100F) u8 encoder_step = 0x01;
RAM_DATA(0x1010) u16 encoder_debounce_mask = 0x0FFF;
RAM_DATA(0x1012) volatile u16 lcd_pump_ptr = (u16)lcd_buf;  /* -> 0x1108 */
RAM_DATA(0x1014) volatile u16 signal_gen_staging_read_ptr = (u16)signal_staging_buf;  /* -> 0x1160 */
RAM_DATA(0x1016) volatile u16 signal_gen_staging_write_ptr = (u16)signal_staging_buf;  /* -> 0x1160 */
RAM_DATA(0x1018) u16 signal_gen_send_buf_read_ptr = (u16)signal_gen_send_buffer;  /* -> 0x1228 */
RAM_DATA(0x101A) u16 signal_gen_send_buf_write_ptr = (u16)signal_gen_send_buffer;  /* -> 0x1228 */
RAM_DATA(0x101C) u16 track_cmd_read_ptr = (u16)track_cmd_buf;  /* -> 0x1268 */
RAM_DATA(0x101E) u16 track_cmd_write_ptr = (u16)track_cmd_buf;  /* -> 0x1268 */
RAM_DATA(0x1020) u8 track_cmd_free = 0x04;
RAM_DATA(0x1021) u16 signal_timer_compare_base = 0x0010;
RAM_DATA(0x1023) char end_of_data_marker[13] = "End of .data";  /* last in the original's .data */

/* The original's .bss, 0x1030-0x1BB6 (cleared by crt_restart); globals.h and lokstack.h
 * describe them */
RAM(0x1030) u8 language_index;
RAM(0x1031) u8 pkt_slot;
RAM(0x1032) u8 pkt_steps;
RAM(0x1033) u8 pkt_kind;
RAM(0x1034) u8 mm_prog_addr;
RAM(0x1035) u8 refresh_slot;
RAM(0x1036) volatile u8 track_scheduler_hold;
RAM(0x1037) u8 mfx_retry;
RAM(0x1038) u8 mfx_slot;
RAM(0x1039) u8 mfx_state;
RAM(0x103A) u8 mfx_substate;
RAM(0x103B) u8 mfx_block;
RAM(0x103C) volatile u8 mfx_cmd_steps;
RAM(0x103D) u8 mfx_read_offset;
RAM(0x103E) u8 mfx_read_len;
RAM(0x103F) u8 mfx_read_buf[2];
RAM(0x1041) u8 mfx_block02_groups;
RAM(0x1042) u8 mfx_block01_cas;
RAM(0x1043) u16 mfx_cv;
RAM(0x1045) u16 mfx_uid_hi;
RAM(0x1047) u16 mfx_uid_lo;
RAM(0x1049) u16 mfx_ca_protocol;
RAM(0x104B) u16 mfx_ca_symbol;
RAM(0x104D) u16 mfx_ca_blocks;
RAM(0x104F) u16 mfx_ca_functions;
RAM(0x1051) u8 mfx_block02;
RAM(0x1052) u8 mfx_block04;
RAM(0x1053) u8 mfx_block05;
RAM(0x1054) u8 mfx_block08;
RAM(0x1055) u8 mfx_block02_cas;
RAM(0x1056) u8 mfx_block04_cas;
RAM(0x1057) u8 mfx_block05_cas;
RAM(0x1058) u8 mfx_block08_cas;
RAM(0x1059) u8 mfx_search_flag;
RAM(0x105A) u8 track_filler_state;
RAM(0x105B) volatile u8 track_filler_hold;
RAM(0x105C) u8 mcan_peer_node;
RAM(0x105D) u16 mcan_peer_handle;
RAM(0x105F) u16 mcan_sd_change_count;
RAM(0x1061) u16 mcan_sy_change_count;
RAM(0x1063) u16 mcan_peer_uid_hi;
RAM(0x1065) u16 mcan_peer_uid_lo;
RAM(0x1067) u8 mcan_reg_mid;
RAM(0x1068) u8 mcan_reg_step;
RAM(0x1069) u16 mcan_peer_timeout;
RAM(0x106B) u16 mcan_list_index;
RAM(0x106D) u8 mcan_link_flags;
RAM(0x106E) u8 mcan_name_send_pos;
RAM(0x106F) u8 can_rx_frame_buf[13];
RAM(0x107C) u16 mcan_rx_handle;
RAM(0x107E) u8 mcan_rx_node;
RAM(0x107F) u8 mcan_rx_cmd;
RAM(0x1080) u8 ee_save_step;
RAM(0x1081) u8 ee_save_item;
RAM(0x1082) u16 ee_record_addr;
RAM(0x1084) u16 ee_record[2];
RAM(0x1088) u16 ee_counter_interval;
RAM(0x108A) u16 ee_counter_ticks;
RAM(0x108C) u8 lcd_text[32];
RAM(0x10AC) u16 lcd_text_offset;
RAM(0x10AE) u16 track_fault_timer;
RAM(0x10B0) u16 menu_value;
RAM(0x10B2) u16 menu_value2;
RAM(0x10B4) u16 menu_value3;
RAM(0x10B6) u16 menu_value4;
RAM(0x10B8) u16 menu_db_brand;
RAM(0x10BA) u8 lok_name_edit_buf[56];
#if MS1_RELEASE != 0x4010
RAM(0x10F2) u16 track_current;
RAM(0x10F4) u16 track_current_offset;
RAM(0x10F6) u16 overload_slow;
RAM(0x10F8) u16 overload_fast;
RAM(0x10FA) u8 shutdown_count;
RAM(0x10FB) u16 shutdown_ticks;
#endif
RAM(0x10FD) u16 encoder_clicks_hi;
RAM(0x10FF) u16 encoder_clicks_lo;
RAM(0x1101) u8 encoder_count;
RAM(0x1102) u8 encoder_mode;
RAM(0x1103) u16 encoder_a_debounce;
RAM(0x1105) u16 encoder_b_debounce;
RAM(0x1107) u8 encoder_ab_state;
RAM(0x1298) u8 track_cmd_pos;
RAM(0x129B) u8 signal_gen_prev_cmd_type;
RAM(0x129C) u8 track_capture_cmd;
RAM(0x129D) u8 track_capture_next;
RAM(0x129E) u16 track_capture_samples;
RAM(0x12A0) u8 track_polarity;
RAM(0x12A1) u8 mfx_stuff_count;
RAM(0x12A2) u8 mfx_bit_pos;
RAM(0x12A3) u8 mfx_byte_idx;
RAM(0x12A4) u8 mfx_crc_state;
RAM(0x13A0) u16 mfx_decode_read_ptr;
RAM(0x13A2) u8 mfx_decode_bits_remaining;
RAM(0x13A3) u8 mfx_decode_current_byte;
RAM(0x13A4) u16 adc_track_voltage;
RAM(0x13A6) u16 adc_decoder_voltage;
RAM(0x13A8) u8 is_master;
RAM(0x13A9) u8 system_status_flags;
RAM(0x13AA) u8 message_ticks;
RAM(0x13AB) u8 message_id;
RAM(0x13AC) u16 ms_tick;
RAM(0x13AE) u8 porta_buttons_raw;
RAM(0x13AF) u8 porta_buttons_edge;
RAM(0x13B0) u8 button_state_raw;
RAM(0x13B1) u8 button_state_edge;
RAM(0x13B2) u8 ui_esc_held;
RAM(0x13B3) u8 ui_stop_held;
RAM(0x13B4) u8 lok_current_slot;
RAM(0x13B5) u8 operating_mode;
RAM(0x13B6) u8 menu_top;
RAM(0x13B7) u8 menu_item;
RAM(0x13B8) u8 menu_sub;
RAM(0x13B9) u8 menu_sub2;
RAM(0x13BA) u8 ui_settings;
RAM(0x13BD) u16 ee_save_pending;
RAM(0x13BF) u16 mcan_status_pending;
RAM(0x13C1) u16 mfx_reg_counter;
RAM(0x13C3) u16 can_device_uid;
RAM(0x13C5) u16 mcan_sd_handle;
RAM(0x13C7) u8 mcan_node;
RAM(0x13C8) u8 ee_counter_record;
RAM(0x13C9) u16 eeprom_record[3][2];
RAM(0x13D5) u16 ee_counter_addr;
RAM(0x13D7) lok_record_t lok_records[LOK_SLOTS + 1];
RAM(0x17F7) lok_state_t lok_states[LOK_SLOTS];
RAM(0x190F) u8 device_name[16];
RAM(0x191F) u16 encoder_last_position;
RAM(0x1921) volatile u16 can_rx_ring_read;
RAM(0x1923) volatile u16 can_rx_ring_write;
RAM(0x1925) volatile u16 can_tx_ring_read;
RAM(0x1927) volatile u16 can_tx_ring_write;
RAM(0x1929) u8 can_tx_ring[130];
RAM(0x19AB) u8 can_rx_ring[416];
RAM(0x1B4B) u8 timer_capture_buffer[96];
RAM(0x1BAB) volatile u16 timer_capture_ptr;
RAM(0x1BAD) volatile u8 timer_capture_count;
RAM(0x1BAE) volatile u8 timer_capture_mode;
RAM(0x1BAF) volatile u16 rtc_day_counter;
RAM(0x1BB1) volatile u16 rtc_day_minutes;
RAM(0x1BB3) volatile u16 rtc_minute_ms;
RAM(0x1BB5) volatile u16 rtc_ms_counter;
