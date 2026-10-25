/*==========================================================================
 * signal/track_filler.c - the track signal when no locomotive needs a packet
 *
 * Functions:
 *   track_filler  (0x5952)
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/track_cmd.h"

extern const u16 flash_hw_id;
extern const u16 flash_sw_id;

/* The next filler: track_filler_state steps through 0-5 (unless track_filler_hold holds
 * it); at 0 the mfx central frame with this device's UID (the flash metadata's software
 * and hardware ID) and mfx_reg_counter, at 2 an MM idle packet, the other states pass on to
 * the next. */
void track_filler(void)
{
    u8 pending;

    for (;;) {
        pending = track_filler_hold;
        if (pending == 0 && ++track_filler_state > 5)
            track_filler_state = pending;
        if (track_filler_state == 0) {
            mfx_frame_central(((u32)flash_sw_id << 16) | flash_hw_id, mfx_reg_counter);
            return;
        }
        if (track_filler_state == 2) {
            mm_write_idle();
            return;
        }
    }
}
