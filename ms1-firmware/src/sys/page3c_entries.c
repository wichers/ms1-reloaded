/*==========================================================================
 * sys/page3c_entries.c - the entry table of flash page $3C, at 0x8000
 *
 * A JMP to each function of PAGE3C_ENTRIES (include/page3c.h), in that order; gcc/page3c.ld
 * puts the section first in the page. The application CALLs the table entries, so the
 * functions behind them may move with every build of the page.
 *========================================================================*/

#include "../include/page3c.h"

#define PAGE3C_JMP(name)    "\tjmp\t" #name "\n"

__asm__(".section .page3c_entry,\"ax\"\n"
        "\t.globl\tpage3c_entry_table\n"
        "page3c_entry_table:\n"
        PAGE3C_ENTRIES(PAGE3C_JMP));
