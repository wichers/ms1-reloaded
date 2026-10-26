/* Linker script for the MS1 firmware, GNU 68HC12 toolchain.
 *
 * The CPU sees one flat 48K code space, 0x4000-0xF7FF: the start-up code maps page $3D into
 * the 0x8000 window once and for all, as the original does. The application's code and its
 * own read-only data take 0x4000-0xE11A (version 4010: 0x4000-0xDFA0), where the original's
 * code and its GCC runtime helpers are; tools/mkimage.py adds what comes from the original
 * image: page $3C (the locomotive database and its accessors), the original's .rodata, build
 * information and configuration area 0xE11B-0xF73F (4010: 0xDFA1-0xF73F; tables the firmware
 * reads by address: language tables, display maps, trit tables, ...), the bootloader and its
 * metadata 0xF800-0xFFFF; and makes the application CRC valid with the word at 0xF73E.
 *
 * This is the template of build/ms1.ld, which tools/ramlayout.py generates from it. The
 * version's part, gcc/<version>/release.ld (the tables' addresses, the end of the
 * application), is found through the linker's search path (-L gcc/<version>). */
OUTPUT_FORMAT("elf32-m68hc12", "elf32-m68hc12", "elf32-m68hc12")
OUTPUT_ARCH(m68hc12)
ENTRY(_start)

MEMORY
{
  ram   (rwx) : ORIGIN = 0x1000, LENGTH = 0x1000
  rom   (rx)  : ORIGIN = 0x4000, LENGTH = 0xE11B - 0x4000
  tramp (rx)  : ORIGIN = 0xF740, LENGTH = 0xC0
}

_stack = 0x2000;

SECTIONS
{
  .text :
  {
    KEEP (*(.crt0))
    *(.text) *(.text.*)
    *(.rodata) *(.rodata.*)
    . = ALIGN(2);
  } > rom

  /* RAM as in the original: its .data at 0x1000 (initial values copied from flash by
   * crt_restart), its .bss from 0x1030, each variable at its address (tools/ramlayout.py
   * fills in the placements), and the GCC soft registers at 0x1BB7. Variables without an
   * address go after the soft registers. */
  .data 0x1000 :
  {
    __data_section_start = .;
    /* @RAM_DATA@ */
    *(.data) *(.data.*)
    _edata = .;
  } AT > rom
  __data_image = LOADADDR(.data);
  ASSERT (_edata == 0x1030, "initialised variable without an address: define it with RAM()")

  .bss 0x1030 (NOLOAD) :
  {
    __bss_start = .;
    /* @RAM_BSS@ */
    *(.softregs)
    *(.bss) *(.bss.*) *(COMMON)
    __bss_end = .;
  } > ram

  .tramp : { KEEP (*(.tramp)) } > tramp

  /DISCARD/ : { *(.comment) }
}

/* Constants the firmware reads from the original image's metadata: the configuration area
 * at 0xF700 and the bootloader's flash metadata at 0xFF10. */
CAN_FILT0_IDAR_LO      = 0xF700;
CAN_FILT0_IDAR_HI      = 0xF702;
CAN_FILT0_IDMR_LO      = 0xF704;
CAN_FILT0_IDMR_HI      = 0xF706;
CAN_FILT1_IDMR_LO      = 0xF70C;
CAN_FILT1_IDMR_HI      = 0xF70E;
flash_str_ptr_serial   = 0xF73A;
flash_app_crc_stored   = 0xF73C;
flash_sw_id            = 0xFF10;
flash_hw_id            = 0xFF12;
flash_vendor_id        = 0xFF16;
flash_vendor_id2       = 0xFF18;
flash_app_crc_expected = 0xFF1A;
flash_build_version    = 0xFF1C;
flash_extra_id1        = 0xFF1E;
flash_extra_id2        = 0xFF20;
flash_compat_flags     = 0xFF2C;
flash_encoder_step    = 0xFF64;

/* The version's tables in the original's .rodata, and the end of the application */
INCLUDE release.ld

/* EEPROM fields the code reads by name (EEPROM at 0x0800, INITEE). */
eeprom_lok_name_dirty  = 0x0808;
eeprom_can_reg_id_hi   = 0x080C;
eeprom_can_reg_id_lo   = 0x080E;

/* More configuration-area and flash-metadata words (some share an address with an
 * alias above). */
CAN_FILT1_IDAR_LO      = 0xF708;
CAN_FILT1_IDAR_HI      = 0xF70A;
flash_ui_stop_param_lo = 0xFF1C;
flash_ui_stop_param_hi = 0xFF1E;
flash_ui_clear_spd_lo  = 0xFF20;
flash_ui_clear_spd_hi  = 0xFF22;
flash_ui_lok_name_lo   = 0xFF28;
flash_ui_lok_name_hi   = 0xFF2A;
flash_adc0_scale       = 0xFF60;
flash_adc1_scale       = 0xFF62;
flash_adc3_scale       = 0xFF66;
flash_adc4_scale       = 0xFF68;
flash_current_limit    = 0xFF7A;
flash_bootloader_entry_ptr = 0xFF70;
