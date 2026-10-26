/* Version 4010 (releases 4010, c010), INCLUDEd by gcc/ms1.ld: the tables in the original's
 * .rodata the code refers to by name, and the end of the application, where that .rodata
 * starts. The tables hold the same bytes as 4011's (the language tables the same strings),
 * at other addresses. */
ASSERT (__data_image + SIZEOF (.data) <= 0xDFA1, "the application runs into the original's .rodata")

eeprom_defaults        = 0xE624;
lang_table             = 0xE0EC;
lok_address_range      = 0xEABA;
lok_range_ec43         = 0xEACE;
lok_param_range        = 0xEAE2;
charset_latin1_map     = 0xEB60;
mfx_icon_map           = 0xE9D4;
rom_func_icon_table    = 0xE9C3;
lok_icon_group_order   = 0xE9F8;
lok_icon_group_keys    = 0xEA08;
lcd_blank_image        = 0xEC92;
lcd_icon_segments      = 0xECEA;
lcd_font               = 0xED90;
charset_index_map      = 0xF190;
lcd_bar_segments       = 0xF2DB;
lcd_text_dots          = 0xF2F7;
lcd_start_image        = 0xF473;
mm_trit_table          = 0xF1E5;
lok_id_table           = 0xEA98;
mm_data_table          = 0xF245;
mfx_cv1_value          = 0xE9C2;
