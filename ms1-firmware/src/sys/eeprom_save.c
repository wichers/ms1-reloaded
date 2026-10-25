/*==========================================================================
 * sys/eeprom_save.c - saving to the EEPROM (0x8DB2-0x9143)
 *
 * The EEPROM is erased and programmed in aligned 4-byte records (build/asm/hal.s). A write
 * stages the record's new content in ee_record and, if it differs from the EEPROM, erases
 * the record and leaves its address in ee_record_addr; eeprom_write_finish programs it
 * once the erase is done.
 *
 * eeprom_save_task (ms_tick phase 3, step 0) writes one record per pass. An item is taken
 * from ee_save_pending, the slot items only while locomotives are controlled
 * (is_master):
 *   bit   item  steps  EEPROM
 *   0x010  11    1     EE_SETTINGS         <- ui_settings
 *   0x020  12    1     EE_LANGUAGE         <- language_index
 *   0x080  14    4     EE_NAME             <- device_name, 16 bytes
 *   0x040  13    1     EE_CURRENT_SLOT     <- lok_current_slot
 *   0x100  15    2     EE_MFX_REG_COUNTER  <- mfx_reg_counter, then EE_SAVE_BUSY cleared
 *   0x400  16    1     EE_MFX_UID          <- mfx_uid_hi, mfx_uid_lo
 * then the first locomotive slot whose state has save bits (SAVE_BITS) set, as
 * item 0-9 in 27 steps: EE_SAVE_BUSY set, the slot's last byte set (invalid), its 24
 * records, EE_SAVE_BUSY cleared (unless the slot changed again: restart at its records).
 * A start-up that finds EE_SAVE_BUSY set counts mfx_reg_counter up (app_main_init).
 * With nothing to save, every ee_counter_interval passes (the interval grows each time)
 * one eeprom_record entry goes to the ring at EE_RECORDS.
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/eeprom_config.h"

/* the changes of a slot's state that are saved */
#define SAVE_BITS     0x38FC

/* Program the record staged by eeprom_write_byte/word/record. */
void eeprom_write_finish(void)
{
    if (ee_record_addr != 0) {
        eeprom_program_word(ee_record_addr, ee_record[0]);
        eeprom_program_word(ee_record_addr + 2, ee_record[1]);
    }
    ee_record_addr = 0;
}

static void stage(const u16 *rec)
{
    if (rec[0] != ee_record[0] || rec[1] != ee_record[1]) {
        ee_record_addr = (u16)rec;
        eeprom_erase_sector((u16)rec);
    }
}

void eeprom_write_byte(u16 addr, u8 value)
{
    const u16 *rec = (const u16 *)(addr & ~3);

    ee_record[1] = rec[1];
    ee_record[0] = rec[0];
    ((u8 *)ee_record)[addr & 3] = value;
    stage(rec);
}

void eeprom_write_word(u16 addr, u16 value)
{
    const u16 *rec = (const u16 *)(addr & ~3);

    ee_record[1] = rec[1];
    ee_record[0] = rec[0];
    *(u16 *)((u8 *)ee_record + (addr & 2)) = value;
    stage(rec);
}

/* addr is record aligned */
void eeprom_write_record(u16 addr, u16 w0, u16 w1)
{
    ee_record[1] = w1;
    ee_record[0] = w0;
    stage((const u16 *)addr);
}

void eeprom_save_task(void)
{
    lok_state_t *d;
    const u16 *src;
    u16 f, w, a;
    u8 step, item, i;

    if (ee_save_step != 0) {
        eeprom_write_finish();
        if (--ee_save_step != 0)
            goto run;
    }
    f = ee_save_pending;
    if (f & 0x10) {
        ee_save_item = 11;
        ee_save_step = 1;
        ee_save_pending = f & ~0x10;
    } else if (f & 0x20) {
        ee_save_item = 12;
        ee_save_step = 1;
        ee_save_pending = f & ~0x20;
    } else if (f & 0x80) {
        ee_save_item = 14;
        ee_save_step = 4;
        ee_save_pending = f & ~0x80;
    } else if (is_master != 0) {
        if (f & 0x40) {
            ee_save_item = 13;
            ee_save_step = 1;
            ee_save_pending = f & ~0x40;
        } else if (f & 0x100) {
            ee_save_item = 15;
            ee_save_step = 2;
            ee_save_pending = f & ~0x100;
        } else if (f & 0x400) {
            ee_save_item = 16;
            ee_save_step = 1;
            ee_save_pending = f & ~0x400;
        } else {
            d = lok_states;
            for (i = 0; i < LOK_SLOTS; i++, d++) {
                w = d->save_pending;
                if (w & SAVE_BITS) {
                    ee_save_item = i;
                    ee_save_step = 27;
                    d->save_pending = w & ~SAVE_BITS;
                    break;
                }
            }
        }
    }

run:
    step = ee_save_step;
    if (step == 0) {
        if (++ee_counter_ticks < ee_counter_interval)
            return;
        ee_counter_ticks = 0;
        ee_counter_interval += 187;
        a = ee_counter_addr - 4;
        if (a < EE_RECORDS)
            a = EE_RECORDS_END - 4;
        eeprom_program_word(a, eeprom_record[ee_counter_record][0]);
        eeprom_program_word(a + 2, eeprom_record[ee_counter_record][1]);
        if (++ee_counter_record > 2)
            ee_counter_record = 0;
        ee_counter_addr += 4;
        if (ee_counter_addr >= EE_RECORDS_END)
            ee_counter_addr = EE_RECORDS;
        eeprom_erase_sector(ee_counter_addr);
        return;
    }

    item = ee_save_item;
    switch (item) {
    case 11:
        eeprom_write_byte(EE_SETTINGS, ui_settings);
        return;
    case 12:
        eeprom_write_byte(EE_LANGUAGE, language_index);
        return;
    case 13:
        eeprom_write_byte(EE_CURRENT_SLOT, lok_current_slot);
        return;
    case 14:
        src = (const u16 *)(device_name + 4 * (step - 1));
        eeprom_write_record(EE_NAME - 4 + 4 * step, src[0], src[1]);
        return;
    case 15:
        if (step == 2)
            eeprom_write_word(EE_MFX_REG_COUNTER, mfx_reg_counter);
        else if (EE_U8(EE_SAVE_BUSY) == 0)
            ee_save_step = 0;
        else
            eeprom_write_word(EE_SAVE_BUSY, 0);
        return;
    case 16:
        eeprom_write_record(EE_MFX_UID, mfx_uid_hi, mfx_uid_lo);
        return;
    }

    /* a locomotive slot */
    if (step == 27) {
        eeprom_write_byte(EE_SAVE_BUSY, 0xFF);
    } else if (step == 26) {
        eeprom_write_byte(EE_SLOTS + item * sizeof(lok_record_t) + LOK_RECORD_OFFSET(unused), 0xFF);
    } else if (step == 1) {
        d = &lok_states[item];
        w = d->save_pending;
        if (w & SAVE_BITS) {
            d->save_pending = w & ~SAVE_BITS;
            ee_save_step = 26;
        } else if (ee_save_pending & 0x100) {
            ee_save_step = 0;
        } else {
            eeprom_write_byte(EE_SAVE_BUSY, 0);
        }
    } else {
        a = (u16)(25 - step) * 4;
        src = (const u16 *)((u8 *)&lok_records[item] + a);
        eeprom_write_record(EE_SLOTS + item * sizeof(lok_record_t) + a, src[0], src[1]);
    }
}
