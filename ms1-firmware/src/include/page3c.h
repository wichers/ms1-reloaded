/*==========================================================================
 * page3c.h - the functions the application calls in flash page $3C
 *
 * Page $3C is linked on its own (gcc/page3c.ld), after the application and against its
 * symbols. A CALL maps it into the window at 0x8000 in place of page $3D for the time of the
 * call, so code there may call and read only the fixed pages, RAM and page $3C itself;
 * tools/banks.py keeps everything it calls, and every interrupt handler, in the fixed pages.
 *
 * The application reaches page $3C only through the entry table at its start: a JMP (3 bytes)
 * per name of PAGE3C_ENTRIES, in this order (src/sys/page3c_entries.c). The application's
 * link gives each name the table entry's address in the linker's banked form, 0x100000 +
 * offset (build/page3c_symbols.ld, tools/banks.py), so GCC's CALL of a far function carries
 * page $3C. A new entry goes at the end, with its far declaration below.
 *========================================================================*/
#ifndef PAGE3C_H
#define PAGE3C_H

#include "hcs12.h"

#define PAGE3C_ENTRIES(X) \
    X(lokdb_p3c_count) \
    X(lokdb_p3c_read)

/* lok/lokdb.c: the locomotive database */
u16  lokdb_p3c_count(void) __attribute__((far));
void lokdb_p3c_read(u16 entry, u8 *dest) __attribute__((far));

#endif /* PAGE3C_H */
