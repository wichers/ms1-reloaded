/*==========================================================================
 * can/mcan_name.c - sending the device name on CAN
 *
 * Functions:
 *   mcan_send_device_name  (0x6F6B)
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/lokstack.h"
#include "../include/mcan.h"
#include "../include/str.h"

/* When mcan_status_pending bit 7 asks for it, device_name goes out from its start: frames of
 * command 0x1880 | mcan_node to mcan_sd_handle with the data {0x02, 0x00,
 * position (word), up to 4 name bytes}, the name with its terminating 0. mcan_name_send_pos is
 * the next position + 1 (0: nothing to send); a full transmit queue leaves the rest for the
 * next call. */
void mcan_send_device_name(void)
{
    u8 frame[8];
    u8 pos;
    u16 n;

    if (LO8(mcan_status_pending) & 0x80) {
        mcan_name_send_pos = 1;
        mcan_status_pending &= ~0x0080;
    }
    while ((pos = mcan_name_send_pos) != 0) {
        n = str_len(device_name) - pos + 2;
        if (n > 4)
            n = 4;
        if (n == 0) {
            mcan_name_send_pos = 0;
            return;
        }
        *(u16 *)&frame[0] = 0x0200;
        *(u16 *)&frame[2] = (u16)pos - 1;
        *(u16 *)&frame[6] = *(u16 *)&device_name[pos + 1];
        *(u16 *)&frame[4] = *(u16 *)&device_name[pos - 1];
        if (mcan_send(mcan_node | 0x1880, mcan_sd_handle, frame, n + 4) == 0)
            return;
        mcan_name_send_pos = pos + (u8)n;
    }
}
