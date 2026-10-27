/*==========================================================================
 * main.c - Firmware entry: the original's main() at 0xB9D6
 *
 * Called by the start-up code (sys/crt0.c) after the .data copy and .bss clear, as
 * main(1, {"main"}); the arguments are unused.
 *========================================================================*/

#include "include/hcs12.h"
#include "include/serial_debug.h"
#include "include/globals.h"
#include "include/mcan.h"
#include "include/ui.h"
#include "include/track_cmd.h"
#include "include/page3c.h"
#include "include/dcc.h"

extern void app_main_init(void);                       /* 0xD34F */
extern void adc_read_and_scale_voltages(void);         /* 0xD750 */
extern void track_current_monitor(void);                /* 0x9333 */
extern void track_scheduler(void);                 /* 0x598D */
extern void wait_for_next_ms_tick(void);               /* 0xD73E */
/* Handlers of ms_tick phase 3, by ms_tick bits 2-4 (jump table 0xEAFD) */
extern void eeprom_save_task(void);            /* 0x8E87 */
extern void button_scan(void);            /* 0xD76F */

/* Initialisation, then the cooperative main loop, one pass per millisecond tick. */
int main(void)
{
#ifdef MS1_DEBUG_SERIAL
    serial_init();
    serial_puts("\r\n=== MS1 ===\r\n");
#endif
    app_main_init();
    for (;;) {
        adc_read_and_scale_voltages();
        track_current_monitor();
        encoder_sample();
        if (is_master != 0)
            signal_gen_send_buf_executor(8);
        if (dcc_svc.state != 0)
            dcc_ack_sample();
        switch (ms_tick & 3) {
        case 0:
            mcan_process();
            break;
        case 1:
            if (is_master != 0)
                signal_gen_cmd_buf_consumer(8);
            break;
        case 2:
            track_scheduler();
            break;
        case 3:
            switch (ms_tick & 0x1C) {
            case 0x00: eeprom_save_task(); break;
            case 0x04: button_scan(); break;
            case 0x08: ui_task(); break;
            case 0x0C: lcd_status_icons(); break;
            case 0x10: lcd_speed_bar(); break;
            case 0x14: ui_screen_text(); break;
            case 0x18: lcd_draw_text(); break;
            }
            break;
        }
        encoder_sample();
        wait_for_next_ms_tick();
    }
}
