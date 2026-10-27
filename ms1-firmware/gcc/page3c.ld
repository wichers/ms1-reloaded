/* Flash page $3C, linked on its own, after the application and against its symbols
 * (build/app_symbols.elf: build/ms1.elf without the entry-table names): the entry table
 * (src/sys/page3c_entries.c) at 0x8000, then the code the application calls there and its
 * read-only data, the locomotive database (generated from third_party/lokdb.tsv).
 * tools/mkimage.py places it in page $3C. Its RAM variables belong to the application
 * (src/globals.c): the page has none of its own.
 *
 * The page is linked at its banked address, 0x100000 for page $3C 0x8000: the linker turns
 * that into 0x8000 for 16-bit references and into page $3C for a far call. */
OUTPUT_FORMAT("elf32-m68hc12", "elf32-m68hc12", "elf32-m68hc12")
OUTPUT_ARCH(m68hc12)
ENTRY(page3c_entry_table)

MEMORY
{
  page3c (rx) : ORIGIN = 0x100000, LENGTH = 0x4000
}

SECTIONS
{
  .page3c :
  {
    KEEP (*(.page3c_entry))
    *(.rodata) *(.rodata.*) *(.text) *(.text.*)
  } > page3c

  .ram (NOLOAD) : { *(.data) *(.data.*) *(.bss) *(.bss.*) *(COMMON) }
  ASSERT (SIZEOF(.ram) == 0, "a variable in page $3C: define it in src/globals.c")

  /DISCARD/ : { *(.comment) }
}
