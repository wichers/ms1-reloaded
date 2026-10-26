/*==========================================================================
 * lok/lokdb_access.c - the application's calls into the locomotive database of page $3C
 *
 * The database and its accessors (lok/lokdb.c) are in flash page $3C, which a CALL maps into
 * the window at 0x8000 in place of page $3D, where application code lies. Interrupts are
 * masked meanwhile, so that no interrupt handler there runs with page $3C in the window.
 *
 * Functions:
 *   lokdb_count  (0xDCFE) - the number of database entries
 *   lokdb_read   (0xDD07) - copy the start of an entry
 *========================================================================*/

#include "../include/hcs12.h"

/* the CALL's page operand: the 64 KiB part decodes the low two bits of PPAGE */
#define LOKDB_PAGE  "0x3C"

u16 lokdb_count(void)
{
    u16 n;

    __asm__ __volatile__ ("pshc\n\torcc\t#0x50\n\tcall\tlokdb_p3c_count," LOKDB_PAGE "\n\tpulc"
                          : "=d" (n) : : "x", "y", "memory");
    return n;
}

/* dest goes on the stack below the saved CCR (lokdb_p3c_read's struct lokdb_read_args) */
void lokdb_read(u16 entry, u8 *dest)
{
    __asm__ __volatile__ ("pshx\n\tpshc\n\torcc\t#0x50\n\tcall\tlokdb_p3c_read," LOKDB_PAGE
                          "\n\tpulc\n\tleas\t2,sp"
                          : "+d" (entry), "+x" (dest) : : "y", "memory");
}
