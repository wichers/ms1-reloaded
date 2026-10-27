/*==========================================================================
 * lok/lokdb.c - the locomotive database accessors, in page $3C
 *
 * Page $3C holds the database (third_party/lokdb.tsv, built into its binary format by
 * tools/lokdb.py) and these two functions, behind the page's entry table
 * (include/page3c.h). The application calls them through lokdb_count and lokdb_read
 * (lok/lokdb_access.c).
 *
 * Functions:
 *   lokdb_p3c_count  (page $3C 0xBB08 in the original) - the number of database entries
 *   lokdb_p3c_read   (page $3C 0xBB0C in the original) - copy the start of an entry
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/page3c.h"

extern const u16 lokdb_entry_count;
extern const u8 *const lokdb_entries[];

u16 lokdb_p3c_count(void)
{
    return lokdb_entry_count;
}

/* the 56 bytes from the start of entry, whatever its length, to dest (lok_name_edit_buf) */
void lokdb_p3c_read(u16 entry, u8 *dest)
{
    const u16 *s = (const u16 *)lokdb_entries[entry];
    u16 *d = (u16 *)dest;
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
