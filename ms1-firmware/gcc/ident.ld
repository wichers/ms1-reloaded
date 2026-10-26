/* The identification area of a release (src/sys/ident.c), linked on its own: the area at
 * 0xF700-0xF73F, where the bootloader and the code read it, and its strings from 0xF680, the
 * end of the application's flash (gcc/ms1.ld). tools/mkimage.py adds it to the image. */
OUTPUT_FORMAT("elf32-m68hc12", "elf32-m68hc12", "elf32-m68hc12")
OUTPUT_ARCH(m68hc12)
ENTRY(ms1_ident)

SECTIONS
{
  .ident_strings 0xF680 : { *(.rodata) *(.rodata.*) }
  .ident 0xF700 : { KEEP (*(.ident)) }
  /DISCARD/ : { *(.comment) }
}
ASSERT (ADDR (.ident_strings) + SIZEOF (.ident_strings) <= 0xF700, "the identification strings run into the area")
ASSERT (SIZEOF (.ident) == 0x40, "the identification area is not 0xF700-0xF73F")
