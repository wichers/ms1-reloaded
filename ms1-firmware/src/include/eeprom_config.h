/*==========================================================================
 * eeprom_config.h - the EEPROM: 1 KiB at 0x0800-0x0BFF (INITEE = 0x09), erased and programmed
 * in aligned 4-byte records (build/asm/hal.s); sys/eeprom_save.c writes it
 *========================================================================*/
#ifndef EEPROM_CONFIG_H
#define EEPROM_CONFIG_H

#include "hcs12.h"

#define EEPROM_BASE_ADDR    0x0800
#define EE_U8(a)            (*(volatile u8 *)(a))
#define EE_U16(a)           (*(volatile u16 *)(a))

/* The layout */
#define EE_LANGUAGE         0x0800  /* 0-3; anything else: the content is rewritten from eeprom_defaults */
#define EE_SETTINGS         0x0801  /* ui_settings */
#define EE_CURRENT_SLOT     0x0804  /* lok_current_slot */
#define EE_SAVE_BUSY        0x0808  /* nonzero while a slot is being saved */
#define EE_MFX_UID          0x080C  /* two words: the mfx UID being registered */
#define EE_NAME             0x0810  /* 16 bytes: device_name */
#define EE_SLOTS            0x0820  /* the locomotive slots, LOK_SLOTS x 0x60 */
#define EE_MFX_REG_COUNTER  0x0BE0  /* mfx_reg_counter; counted up when the defaults are written */
#define EE_RECORDS          0x0BE4  /* ring of 4-byte records: two words, kind 1-3 in the last byte */
#define EE_RECORDS_END      0x0BFC

/* Default image of 0x0800-0x099F, in the original's read-only data */
extern const u8 eeprom_defaults[0x1A0];   /* 0xE788 */

/* EEPROM commands (build/asm/hal.s) */
void eeprom_erase_sector(u16 addr);
void eeprom_program_word(u16 addr, u16 data);


#endif /* EEPROM_CONFIG_H */
