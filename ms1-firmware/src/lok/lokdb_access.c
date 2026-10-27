/*==========================================================================
 * lok/lokdb_access.c - the application's calls into the locomotive database of page $3C
 *
 * The database and its accessors (lok/lokdb.c) are in flash page $3C; a far call maps it
 * into the window at 0x8000 in place of page $3D for the time of the call
 * (include/page3c.h). No interrupt handler lies in the window, so interrupts stay on.
 *
 * Functions:
 *   lokdb_count  (0xDCFE) - the number of database entries
 *   lokdb_read   (0xDD07) - copy the start of an entry
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/page3c.h"

u16 lokdb_count(void)
{
    return lokdb_p3c_count();
}

void lokdb_read(u16 entry, u8 *dest)
{
    lokdb_p3c_read(entry, dest);
}
