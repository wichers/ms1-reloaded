/* Flash page $3C, linked on its own at the PPAGE window (0x8000-0xBFFF): the locomotive
 * database (generated from third_party/lokdb.tsv) and its accessors (src/lok/lokdb.c).
 * tools/mkimage.py places it in page $3C; the Makefile gives the application the accessors'
 * addresses for the far calls in lok/lokdb_access.c (build/page3c_symbols.ld). */
OUTPUT_FORMAT("elf32-m68hc12", "elf32-m68hc12", "elf32-m68hc12")
OUTPUT_ARCH(m68hc12)
ENTRY(lokdb_p3c_count)

MEMORY
{
  page3c (rx) : ORIGIN = 0x8000, LENGTH = 0x4000
}

SECTIONS
{
  .page3c : { *(.rodata) *(.rodata.*) *(.text) *(.text.*) } > page3c
  /DISCARD/ : { *(.comment) }
}
