/*==========================================================================
 * lok/lokdb.c - the locomotive database accessors, in page $3C
 *
 * Page $3C holds the database (third_party/lokdb.tsv, built into its binary format by
 * tools/lokdb.py) and these two functions; it is linked on its own (gcc/page3c.ld) and is
 * not part of the application. The application reaches them through lokdb_count and
 * lokdb_read (lok/lokdb_access.c), which mask interrupts and CALL them with page $3C in the
 * window: they must not call anything outside the page.
 *
 * Functions:
 *   lokdb_p3c_count  (page $3C 0xBB08) - the number of database entries
 *   lokdb_p3c_read   (page $3C 0xBB0C) - copy the start of an entry
 *========================================================================*/

#include "../include/hcs12.h"

extern const u16 lokdb_entry_count;
extern const u8 *const lokdb_entries[];

/* What lies on the stack above the CALL's return address when lokdb_read calls: the CCR it
 * saved, then the destination */
struct lokdb_read_args {
    u8  ccr;
    u8  *dest;
};

u16 lokdb_p3c_count(void) __attribute__((far));
void lokdb_p3c_read(u16 entry, struct lokdb_read_args args) __attribute__((far));

u16 lokdb_p3c_count(void)
{
    return lokdb_entry_count;
}

/* the 56 bytes from the start of entry, whatever its length, to args.dest
 * (lok_name_edit_buf). Interrupts are masked meanwhile, so the track signal interrupt waits
 * for it: four words per pass keep the copy below the original's time (ms1emu -I). */
void lokdb_p3c_read(u16 entry, struct lokdb_read_args args)
{
    const u16 *s = (const u16 *)lokdb_entries[entry];
    u16 *d = (u16 *)args.dest;
    u8 n = 7;

    do {
        d[0] = s[0];
        d[1] = s[1];
        d[2] = s[2];
        d[3] = s[3];
        d += 4;
        s += 4;
    } while (--n);
}
