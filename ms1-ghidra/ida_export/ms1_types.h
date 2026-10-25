/* Local types exported from ms1_firmware.i64 */

#define __int8 char
#define __int16 short
#define __int32 int
#define __int64 long long

/* 1 */
#pragma pack(push, 2)
struct lokstack_entry_t
{
  unsigned __int8 active;
  unsigned __int8 flags;
  unsigned int lok_address;
  unsigned __int8 protocol;
  unsigned __int8 speed_steps;
  unsigned __int8 name[10];
  unsigned __int8 obj_type;
  unsigned __int8 decoder_protocol;
  unsigned int match_address;
  unsigned int function_icons;
  unsigned __int8 function_count;
  unsigned __int8 current_speed;
  unsigned __int8 direction;
  unsigned __int8 function_state[16];
  unsigned __int8 padding2[3];
  unsigned __int8 lok_symbol;
  unsigned __int8 param_vmax;
  unsigned __int8 param_acc;
  unsigned __int8 param_dec;
  unsigned __int8 param_vol;
  unsigned __int8 reserved[45];
  unsigned __int8 match_flags;
  unsigned __int8 can_sync_flags;
};
#pragma pack(pop)

/* 2 */
#pragma pack(push, 2)
struct lokstack_display_t
{
  unsigned int slot_address;
  unsigned __int8 direction_byte;
  unsigned __int8 func_bits_hi;
  unsigned __int8 func_bits_lo;
  unsigned __int8 render_flags;
  unsigned __int8 function_display[8];
  unsigned int ext_flags;
  unsigned int func_event_mask;
  unsigned int ctrl_flags;
  unsigned int func_update_mask;
  unsigned __int8 _pad_16;
  unsigned __int8 disp_counter_hi;
  unsigned __int8 disp_counter_lo;
  unsigned __int8 timing_state;
  unsigned int scroll_anim_state;
};
#pragma pack(pop)

/* 3 */
#pragma pack(push, 2)
struct ui_context_t
{
  unsigned __int8 mode;
  unsigned __int8 submode;
  unsigned int state;
  unsigned int current_speed;
  unsigned int current_address;
  unsigned int lok_slot_index;
  unsigned int display_flags;
  unsigned int lokstack_ptr;
  unsigned int lokentry_ptr;
  unsigned int display_ptr;
  unsigned int lokdisplay_ptr;
  unsigned __int8 lok_active;
};
#pragma pack(pop)

/* 4 */
#pragma pack(push, 2)
struct lok_db_entry_t
{
  unsigned __int8 entry_type;
  unsigned __int8 flags;
  unsigned int lok_address;
  unsigned __int8 protocol;
  unsigned __int8 speed_steps;
  unsigned __int8 name[10];
  unsigned __int8 decoder_type;
  unsigned int function_icons;
  unsigned __int8 symbol_index;
};
#pragma pack(pop)

/* 5 */
#pragma pack(push, 2)
struct can_frame_t
{
  unsigned int id_high;
  unsigned int id_low;
  unsigned __int8 dlc;
  unsigned __int8 data[8];
};
#pragma pack(pop)

/* 6 */
#pragma pack(push, 2)
struct eeprom_config_t
{
  unsigned __int8 magic;
  unsigned __int8 version;
  unsigned __int8 language;
  unsigned __int8 contrast;
  unsigned int last_lok_address;
  unsigned __int8 last_lok_slot;
  unsigned __int8 lok_count;
  unsigned __int8 reserved[8];
};
#pragma pack(pop)

/* 7 */
#pragma pack(push, 2)
struct bci_block_header_t
{
  unsigned __int8 magic_09;
  unsigned __int8 block_size_10;
  unsigned __int8 key_a;
  unsigned __int8 key_b;
  unsigned __int8 dest_page;
  unsigned __int8 reserved[2];
  unsigned __int8 payload_size_lo;
  unsigned __int8 payload_size_hi;
  unsigned __int8 descramble_seed;
  unsigned __int8 descramble_key;
  unsigned __int8 padding[3];
};
#pragma pack(pop)

/* 8 */
#pragma pack(push, 2)
struct bci_flash_state_t
{
  unsigned int flash_buf_ptr;
  unsigned __int8 flash_page_addr;
  unsigned __int8 flash_flags;
  unsigned __int8 flash_handle_prio;
  unsigned int flash_uid_hi;
  unsigned int flash_uid_lo;
  unsigned __int8 flash_can_prio;
  unsigned int flash_can_id_high;
  unsigned int flash_can_id_low;
  unsigned int flash_can_data0;
  unsigned int flash_can_data2;
  unsigned int flash_can_data4;
  unsigned int flash_can_data6;
  unsigned __int8 flash_uid_match;
  unsigned __int8 descramble_state;
  unsigned __int8 descramble_key;
  unsigned int flash_dest_addr;
  unsigned int crc16_state;
};
#pragma pack(pop)

/* 9 */
#pragma pack(push, 2)
struct bci_can_flash_id_t
{
  unsigned __int8 priority;
  unsigned __int8 flash_handle;
  unsigned __int8 request_bit;
  unsigned __int8 flash_stage;
  unsigned __int8 flash_flags;
  unsigned __int8 node_number;
};
#pragma pack(pop)

/* 10 */
#pragma pack(push, 2)
struct mcan_registration_id_t
{
  unsigned __int16 priority : 3;
  unsigned __int16 uid_byte : 10;
  unsigned __int16 stage : 3;
  unsigned __int16 mid : 5;
  unsigned __int16 direction : 1;
  unsigned __int16 node_number : 7;
};
#pragma pack(pop)

/* 11 */
#pragma pack(push, 2)
struct mcan_operational_id_t
{
  unsigned __int16 priority : 3;
  unsigned __int16 object_handle : 16;
  unsigned __int16 command : 3;
  unsigned __int16 node_number : 7;
};
#pragma pack(pop)

/* 12 */
#pragma pack(push, 2)
struct mcan_flash_id_t
{
  unsigned __int16 priority : 3;
  unsigned __int16 flash_handle : 8;
  unsigned __int16 request : 3;
  unsigned __int16 flash_stage : 5;
  unsigned __int16 flash_flags : 3;
  unsigned __int16 node_number : 7;
};
#pragma pack(pop)

/* 13 */
#pragma pack(push, 2)
struct mcan_reg_stage0_master_t
{
  unsigned __int8 uid_master[4];
  unsigned __int8 zero1;
  unsigned __int8 master_type;
  unsigned __int8 zero2;
  unsigned __int8 zero3;
};
#pragma pack(pop)

/* 14 */
#pragma pack(push, 2)
struct mcan_reg_stage1_master_t
{
  unsigned __int8 zero1[3];
  unsigned __int8 vendor_id_nmra;
  unsigned __int8 master_subtype;
  unsigned __int8 zero2;
  unsigned __int8 power_byte;
  unsigned __int8 protocol_byte;
};
#pragma pack(pop)

/* 15 */
#pragma pack(push, 2)
struct mcan_reg_stage2_master_t
{
  unsigned __int8 bl_version[2];
  unsigned __int8 zero1;
  unsigned __int8 power_rating;
  unsigned __int8 zero2;
  unsigned __int8 zero3;
  unsigned __int8 hw_version_hi;
  unsigned __int8 hw_version_lo;
};
#pragma pack(pop)

/* 16 */
#pragma pack(push, 2)
struct mcan_reg_stage3_master_t
{
  unsigned __int8 zero1[2];
  unsigned __int8 sw_version_hi;
  unsigned __int8 sw_version_lo;
  unsigned __int8 fw_version_hi;
  unsigned __int8 fw_version_lo;
  unsigned __int8 zero2;
  unsigned __int8 extra_flags;
};
#pragma pack(pop)

/* 17 */
#pragma pack(push, 2)
struct mcan_reg_stage4_slave_t
{
  unsigned __int8 uid_slave[4];
  unsigned __int8 zero1;
  unsigned __int8 descriptor_count;
  unsigned __int8 zero2;
  unsigned __int8 descriptor_type;
};
#pragma pack(pop)

/* 18 */
#pragma pack(push, 2)
struct mcan_reg_stage5_slave_t
{
  unsigned __int8 zero1[3];
  unsigned __int8 vendor_id_nmra;
  unsigned __int8 hw_id_hi;
  unsigned __int8 hw_id_lo;
  unsigned __int8 sw_id_hi;
  unsigned __int8 sw_id_lo;
};
#pragma pack(pop)

/* 19 */
#pragma pack(push, 2)
struct mcan_reg_stage6_slave_t
{
  unsigned __int8 bl_hi;
  unsigned __int8 bl_lo;
  unsigned __int8 zero1;
  unsigned __int8 build_version;
  unsigned __int8 zero2;
  unsigned __int8 zero3;
  unsigned __int8 sw_id_hi;
  unsigned __int8 sw_id_lo;
};
#pragma pack(pop)

/* 20 */
#pragma pack(push, 2)
struct mcan_reg_stage7_master_t
{
  unsigned __int8 uid_slave[4];
  unsigned __int8 start_handle_hi;
  unsigned __int8 start_handle_lo;
  unsigned __int8 node_number;
  unsigned __int8 flash_flag;
};
#pragma pack(pop)

/* 21 */
#pragma pack(push, 2)
struct mcan_lok_speed_t
{
  unsigned __int8 speed_value;
  unsigned __int8 direction;
};
#pragma pack(pop)

/* 22 */
#pragma pack(push, 2)
struct mcan_lok_function_t
{
  unsigned __int8 function_index;
  unsigned __int8 function_state;
};
#pragma pack(pop)

/* 23 */
#pragma pack(push, 2)
struct mcan_lok_name_t
{
  unsigned __int8 name_chars[8];
};
#pragma pack(pop)

/* 24 */
#pragma pack(push, 2)
struct mcan_system_cmd_t
{
  unsigned __int8 cmd_type;
  unsigned __int8 subcmd;
  unsigned __int8 data[6];
};
#pragma pack(pop)

/* 25 */
#pragma pack(push, 2)
struct mcan_flash_request_t
{
  unsigned __int8 offset[4];
  unsigned __int8 count[4];
};
#pragma pack(pop)

/* 26 */
#pragma pack(push, 2)
struct mcan_flash_data_t
{
  unsigned __int8 offset[4];
  unsigned __int8 flash_data[4];
};
#pragma pack(pop)

/* 27 */
#pragma pack(push, 2)
struct mcan_flash_end_t
{
  unsigned __int8 uid_slave[4];
};
#pragma pack(pop)

/* 28 */
#pragma pack(push, 2)
struct mcan_lok_object_t
{
  unsigned int lok_address;
  unsigned __int8 protocol_type;
  unsigned __int8 speed_steps;
  unsigned __int8 name[10];
  unsigned __int8 decoder_type;
  unsigned __int8 function_count;
  unsigned int function_icons;
  unsigned __int8 symbol_index;
  unsigned __int8 param_vmax;
  unsigned __int8 param_acc;
  unsigned __int8 param_dec;
  unsigned __int8 param_vol;
};
#pragma pack(pop)

/* 29 */
struct lokdb_raw_header_t
{
  unsigned int lok_address;
  unsigned __int8 config_flags;
  unsigned __int8 traction_type;
};

/* 30 */
struct lokdb_record_std_t
{
  unsigned int lok_address;
  unsigned __int8 config_flags;
  unsigned __int8 traction_type;
  unsigned __int8 speed_steps;
  unsigned __int8 function_count;
};

/* 31 */
struct lokdb_record_short_t
{
  unsigned int lok_address;
  unsigned __int8 config_flags;
  unsigned __int8 traction_type;
  unsigned __int8 speed_steps;
};

/* 33 */
struct lokdb_record_mfx_t
{
  unsigned int lok_address;
  unsigned __int8 config_flags;
  unsigned __int8 traction_type;
  unsigned __int8 speed_steps;
  unsigned __int8 function_count;
  unsigned __int8 func_icon_0;
  unsigned __int8 func_icon_1;
  unsigned __int8 func_icon_2;
  unsigned __int8 name_marker;
};

