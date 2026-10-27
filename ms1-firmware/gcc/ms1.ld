/* Linker script for the MS1 firmware, GNU 68HC12 toolchain.
 *
 * The CPU sees one flat 48K code space, 0x4000-0xF7FF: the start-up code maps page $3D into
 * the 0x8000 window, as the original does. The application's code and read-only data take
 * 0x4000 up to the identification strings; tools/mkimage.py adds page $3C and the release's
 * identification area (src/sys/ident.c), each linked on its own, and the bootloader of a
 * device dump; and makes the application CRC valid with the word at 0xF73E.
 *
 * Page $3C replaces page $3D in the window while the application runs code there
 * (include/page3c.h), with interrupts on. So the code that may run then lies in the fixed
 * pages: tools/banks.py lists the interrupt handlers, what page-$3C code calls, and what
 * those call, and they come first, from 0x4000 on (@FIXED_TEXT@); read-only data follows the
 * code, above the window.
 *
 * This is the template of build/ms1.ld, which tools/ramlayout.py (RAM) and tools/banks.py
 * (fixed code) generate from it. The addresses of the page-$3C entry table
 * (build/page3c_symbols.ld) come from tools/banks.py, found through the linker's search path
 * (-L build). */
OUTPUT_FORMAT("elf32-m68hc12", "elf32-m68hc12", "elf32-m68hc12")
OUTPUT_ARCH(m68hc12)
ENTRY(_start)

MEMORY
{
  ram   (rwx) : ORIGIN = 0x1000, LENGTH = 0x1000
  rom   (rx)  : ORIGIN = 0x4000, LENGTH = 0xF680 - 0x4000   /* the identification strings follow (gcc/ident.ld) */
  tramp (rx)  : ORIGIN = 0xF740, LENGTH = 0xC0
}

_stack = 0x2000;

SECTIONS
{
  .text :
  {
    KEEP (*(.crt0))
    *(.text)
    /* @FIXED_TEXT@ */
    __fixed_end = .;
    *(.text.*)
    __rodata_start = .;
    *(.rodata) *(.rodata.*)
    . = ALIGN(2);
  } > rom
  ASSERT (__fixed_end <= 0x8000, "the code that must stay in fixed flash reaches the window")
  ASSERT (__rodata_start >= 0xC000, "read-only data in the window")

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

/* The bootloader's flash metadata words the code reads by name (docs/bootloader.md: The
 * metadata block); the code reads the others through their address. */
flash_uid_hi           = 0xFF10;
flash_uid_lo           = 0xFF12;
flash_hw_version       = 0xFF18;
flash_hw_code          = 0xFF1A;
flash_compat_flags     = 0xFF2C;
flash_adc0_scale       = 0xFF60;
flash_adc1_scale       = 0xFF62;
flash_encoder_step     = 0xFF64;
flash_adc3_scale       = 0xFF66;
flash_adc4_scale       = 0xFF68;
flash_current_limit    = 0xFF7A;

/* The page-$3C entry table (include/page3c.h) */
INCLUDE page3c_symbols.ld
