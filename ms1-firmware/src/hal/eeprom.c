/*==========================================================================
 * hal/eeprom.c - the EEPROM's commands: sector erase and word program
 *
 * An erase is started and left running: its callers wait (delay_short) before they program
 * the sector. A program waits until the command has completed.
 *
 * Functions:
 *   eeprom_erase_sector  (0xDCCF) - start erasing the 4-byte sector of addr
 *   eeprom_program_word  (0xDCE8) - program a word, wait for it
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/eeprom_config.h"

#define ECLKDIV_EDIVLD  0x80        /* the clock divider has been written */
#define ECLKDIV_24MHZ   0x16        /* the command clock from the 24 MHz bus */
#define ECMD_PROGRAM    0x20
#define ECMD_ERASE      0x40
#define ESTAT_CBEIF     0x80        /* write 1: launch the command */
#define ESTAT_CCIF      0x40        /* the commands have completed */

void eeprom_erase_sector(u16 addr)
{
    if (!(ECLKDIV & ECLKDIV_EDIVLD))
        ECLKDIV = ECLKDIV_24MHZ;
    EE_U16(addr) = addr;            /* the write selects the sector; its data does not matter */
    ECMD = ECMD_ERASE;
    ESTAT = ESTAT_CBEIF;
}

void eeprom_program_word(u16 addr, u16 data)
{
    EE_U16(addr) = data;
    ECMD = ECMD_PROGRAM;
    ESTAT = ESTAT_CBEIF;
    while (!(ESTAT & ESTAT_CCIF))
        ;
}
