/* Version 4011 (releases 4011, c011), INCLUDEd by gcc/ms1.ld: the tables in the original's
 * .rodata the code refers to by name, and the end of the application, where that .rodata
 * starts. */
ASSERT (__data_image + SIZEOF (.data) <= 0xE11B, "the application runs into the original's .rodata")

eeprom_defaults        = 0xE788;
lang_table             = 0xE250;
lok_address_range      = 0xEC2F;
lok_range_ec43         = 0xEC43;
lok_param_range        = 0xEC57;
charset_latin1_map     = 0xECD5;
mfx_icon_map           = 0xEB49;
rom_func_icon_table    = 0xEB38;
lok_icon_group_order   = 0xEB6D;
lok_icon_group_keys    = 0xEB7D;
lcd_blank_image        = 0xEE07;
lcd_icon_segments      = 0xEE5F;
lcd_font               = 0xEF05;
charset_index_map      = 0xF305;
lcd_bar_segments       = 0xF450;
lcd_text_dots          = 0xF46C;
lcd_start_image        = 0xF5E8;
mm_trit_table          = 0xF35A;
lok_id_table           = 0xEC0D;
mm_data_table          = 0xF3BA;
mfx_cv1_value          = 0xEB37;
