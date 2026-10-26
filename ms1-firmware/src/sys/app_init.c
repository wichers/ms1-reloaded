/*==========================================================================
 * sys/app_init.c - Application start-up
 *
 * Functions:
 *   app_main_init                (0xD34F) - start-up: hardware, EEPROM, locomotive slots, CAN
 *   adc_read_and_scale_voltages  (0xD750) - track and feedback voltages
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/str.h"
#include "../include/eeprom_config.h"
#include "../include/rom_tables.h"
#include "../include/lcd.h"
#include "../include/ui.h"
#include "../include/track_cmd.h"
#include "../include/mcan.h"
#include "../include/delay.h"

/* The EEPROM's default contents, 0x0800-0x099F (the comments: EEPROM addresses) */
static const u8 eeprom_defaults[0x1A0] = {
    0x01, 0x08, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x0800 */
    0x4D, 0x4F, 0x42, 0x49, 0x4C, 0x45, 0x20, 0x53, 0x54, 0x41, 0x54, 0x49, 0x4F, 0x4E, 0x00, 0x00,   /* 0x0810 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x0820 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x0830 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x0840 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x0850 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x0860 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x0870 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x0880 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x0890 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x08A0 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x08B0 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x08C0 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x08D0 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x08E0 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x08F0 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x0900 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x0910 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x0920 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x0930 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x0940 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x0950 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x0960 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x0970 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x0980 */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   /* 0x0990 */
};


extern void timer_atd_init(void);   /* 0xBA5E */
extern void timer_modulus_init(void);
extern u16  timer_modulus_read(void);
extern void mfx_registration_start(void);

/* 0xD750: the track voltage and the decoder feedback voltage (ATD0 channels 0 and 1),
 * scaled by the factors in the bootloader's flash metadata */
void adc_read_and_scale_voltages(void)
{
    u16 scale;

    scale = flash_adc0_scale;
    if (scale == 0)
        scale = 0x6C4F;
    adc_track_voltage = (u16)(((u32)scale * ATD0DR0) >> 16);
    scale = flash_adc1_scale;
    if (scale == 0)
        scale = 0x6C4F;
    adc_decoder_voltage = (u16)(((u32)scale * ATD0DR1) >> 16);
}

/* 0xD34F */
void app_main_init(void)
{
    s16 off;
    u16 n, empty, k;
    u16 addr;
    u16 *rec, *last_empty;
    u8 b, master;
    u16 slot;
    lok_state_t *disp;

    IRQ_DISABLE();
    lcd_init();
    lcd_buf_init();
    lcd_pump_all();
    PORTE |= 0x04;
    timer_atd_init();
    timer_modulus_init();
    delay_short(4);
    adc_read_and_scale_voltages();
    /* track voltage present: this MS1 is the master and generates the track signal */
    if (adc_track_voltage > 5000)
        is_master = 1;
    can_device_uid = (flash_uid_lo ^ mfx_reg_counter) | 1;
    if (is_master != 0) {
        signal_gen_init();
        mcan_sd_handle = 0x100;
        system_status_flags = 1;
    } else {
        system_status_flags = 0x11;
    }

    /* EEPROM content this firmware does not know (the language byte above 3, e.g. erased):
     * erase the slot area beyond the default image, count up the node ID, write the
     * default image */
    if (EE_U8(EE_LANGUAGE) > 3) {
        off = EE_MFX_REG_COUNTER - EEPROM_BASE_ADDR;
        while ((s16)sizeof eeprom_defaults < off) {
            off -= 4;
            eeprom_erase_sector(EEPROM_BASE_ADDR + off);
            delay_short(0x20);
        }
        mfx_reg_counter = EE_U16(EE_MFX_REG_COUNTER);
        eeprom_erase_sector(EE_MFX_REG_COUNTER);
        delay_short(0x20);
        eeprom_program_word(EE_MFX_REG_COUNTER, mfx_reg_counter + 1);
        eeprom_program_word(EE_MFX_REG_COUNTER + 2, 0);
        while (off > 0) {
            off -= 4;
            addr = EEPROM_BASE_ADDR + off;
            eeprom_erase_sector(addr);
            delay_short(0x20);
            eeprom_program_word(addr, *(const u16 *)&eeprom_defaults[off]);
            eeprom_program_word(off + EEPROM_BASE_ADDR + 2, *(const u16 *)&eeprom_defaults[off + 2]);
        }
    }

    /* Record ring (EE_RECORDS, 4-byte records, one or two erased): find the erased ones */
    empty = 0;
    last_empty = (u16 *)ee_counter_addr;
    rec = (u16 *)EE_RECORDS;
    do {
        if (rec[0] == 0xFFFF && rec[1] == 0xFFFF) {
            empty++;
            last_empty = rec;
        }
        rec += 2;
    } while (rec != (u16 *)EE_RECORDS_END);
    ee_counter_addr = (u16)last_empty;
    if (empty == 0 || (s16)empty > 2) {
        /* not a valid ring: start a new one */
        for (addr = EE_RECORDS; (s16)addr <= (s16)(EE_RECORDS_END - 4); addr += 4) {
            eeprom_erase_sector(addr);
            delay_short(0x20);
        }
        for (k = 0; (s16)k <= 5; k++) {
            if ((s16)k <= 2) {
                eeprom_program_word(EE_RECORDS + k * 4, 0);
                eeprom_program_word(EE_RECORDS + 2 + k * 4, k + 1);
            } else if ((s16)k > 4) {
                eeprom_program_word(EE_RECORDS + k * 4, 0);
                eeprom_program_word(EE_RECORDS + 2 + k * 4, 0);
            }
        }
        ee_counter_addr = EE_RECORDS_END - 8;
    } else {
        /* keep one erased record behind the newest */
        eeprom_erase_sector((u16)last_empty);
        delay_short(0x20);
        addr = ee_counter_addr;
        if (addr == EE_RECORDS_END - 4 && EE_U16(EE_RECORDS) == 0xFFFF && EE_U16(EE_RECORDS + 2) == 0xFFFF) {
            ee_counter_addr = EE_RECORDS;
            addr = EE_RECORDS;
        } else if (addr == EE_RECORDS) {
            addr = EE_RECORDS_END - 4;
        } else {
            addr -= 4;
        }
        eeprom_erase_sector(addr);
        delay_short(0x20);
    }

    /* The newest record of each kind (1-3, in its last byte) behind the erased one */
    rec = (u16 *)ee_counter_addr;
    k = 0;
    n = 4;
    do {
        if (rec == (u16 *)EE_RECORDS)
            rec = (u16 *)EE_RECORDS_END;
        rec -= 2;
        if (k != 0) {
            b = ((u8 *)rec)[3];
            if (b != 0 && b <= 3) {
                ee_counter_record = b - 1;
                eeprom_record[b - 1][1] = rec[1];
                eeprom_record[b - 1][0] = rec[0];
            }
        }
        k++;
    } while (--n);

    language_index = EE_U8(EE_LANGUAGE);
    ui_settings = EE_U8(EE_SETTINGS);
    if (is_master != 0) {
        lok_current_slot = EE_U8(EE_CURRENT_SLOT);
        if (EE_U16(EE_MFX_UID) != 0 || EE_U16(EE_MFX_UID + 2) != 0)
            mfx_registration_start();
    }
    mfx_reg_counter = EE_U16(EE_MFX_REG_COUNTER);
    if (EE_U8(EE_SAVE_BUSY) != 0) {
        mfx_reg_counter = EE_U16(EE_MFX_REG_COUNTER) + 1;
        ee_save_pending |= 0x100;
    }
    str_ncpy((const u8 *)EE_NAME, device_name, 0x10);

    /* Locomotive slots: the master restores them from EEPROM, a slave marks them free */
    master = is_master;
    disp = lok_states;
    for (slot = 0; slot < LOK_SLOTS; slot++, disp++) {
        lok_record_t *entry = &lok_records[slot];
        if (master == 0) {
            entry->unused = 0xFF;
            continue;
        }
        {
            const u8 *src = (const u8 *)EE_SLOTS + slot * sizeof(lok_record_t);
            u8 *dst = (u8 *)entry;
            do
                *dst++ = *src++;
            while (dst != (u8 *)entry + sizeof(lok_record_t));
        }
        disp->object = slot + 0x200;
        lok_states[slot].change_count = 1;
        disp->flags = entry->protocol == LOK_MFX ? LOKS_NO_ANSWER : LOKS_NO_TRACK;
        if (entry->protocol == LOK_MFX && entry->mfx_uid_hi == 0xFFFF && entry->mfx_uid_lo == 0xFFFF)
            entry->unused = 0xFF;
    }

    lok_edit.unused = 0xFF;
    encoder_clicks_lo = eeprom_record[2][1];
    encoder_clicks_hi = eeprom_record[2][0];
    if (flash_hw_version <= 0x101 && flash_compat_flags == 0 && flash_uid_hi >= 2 &&
        (flash_uid_hi > 2 || flash_uid_lo > 0x62B7))
        encoder_debounce_mask = 0x1F;
    ui_enter_mode(0);
    delay_short(500);
    can_controller_full_init(0x9C4);
    lcd_buf_load();
    timer_modulus_read();
    IRQ_ENABLE();
}
