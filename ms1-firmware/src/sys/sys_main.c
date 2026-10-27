/*==========================================================================
 * sys/sys_main.c - messages, the main task scheduler
 *
 * Functions:
 *   show_message            (0xDCA9) - show a message for a while
 *   track_scheduler     (0x598D) - ms_tick phase 2 work dispatcher
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/lokstack.h"
#include "../include/track_cmd.h"
#include "../include/page3c.h"
#include "../include/dcc.h"

/* Show a message for ticks UI passes: system_status_flags bit 7 while it shows, the
 * display (ui/screen_text.c) shows LANG(0x16 + message_id), the UI state machine
 * (ui_task) counts message_ticks down. A message already showing is
 * replaced, unless its ticks ran out. */
void show_message(u8 message, u8 ticks)
{
    u8 st = system_status_flags;

    if ((st & 0x80) && message_ticks == 0)
        return;
    message_id = message;
    message_ticks = ticks;
    system_status_flags = st | 0x80;
}

/* The callees each return nonzero when they did the work of this pass */
extern u16  lok_packets_refresh(void);
extern u16  mfx_registration_process(void);
extern u16  lok_packets_next(void);

/* ms_tick phase 2: while track_cmd_free is nonzero, give each pass to the first of
 * track_filler (unless track_filler_hold), lok_packets_next (unless track_scheduler_hold, or
 * an mfx registration command is under way), mfx_registration_process (unless
 * track_scheduler_hold) and
 * lok_packets_refresh that does work, track_filler when none does; DCC service mode
 * (signal/dcc_service.c) has all of them. As slave (no track signal) it clears the slots'
 * track work (track_pending, track_functions). */
void track_scheduler(void)
{
    lok_state_t *d;
    u16 n;
    u8 pending;

    if (is_master == 0) {
        d = lok_states;
        for (n = 10; n != 0; n--, d++) {
            d->track_pending = 0;
            d->track_functions = 0;
        }
        return;
    }
    while (track_cmd_free != 0) {
        if (dcc_svc.state != 0) {               /* DCC service mode has the track */
            dcc_service_next();
            continue;
        }
        pending = track_filler_hold;
        if (!pending) {
            if (track_scheduler_hold == 0 && mfx_cmd_steps == 0) {
                if (lok_packets_next())
                    continue;
                pending = track_filler_hold;
            }
            if (!pending) {
                if (track_scheduler_hold == 0) {
                    if (mfx_registration_process())
                        continue;
                    pending = track_filler_hold;
                }
                if (!pending && lok_packets_refresh())
                    continue;
            }
        }
        track_filler();
    }
}
