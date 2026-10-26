/*==========================================================================
 * mcan.h - The MS1's CAN protocol (Sources/can/)
 *========================================================================*/
#ifndef MCAN_H
#define MCAN_H

#include "hcs12.h"
#include "lokstack.h"

/* can/can0.c: the CAN0 controller and its frame rings */
void can_controller_full_init(u16 bitrate_100);                  /* 0xDDB5: bit rate in 100 bit/s */
u16  can_tx_enqueue_frame(u32 id, const u8 *data, u16 dlc);      /* 0xDE43 */
u16  can_rx_dequeue_frame(u8 *dest);                             /* 0xDEAC */

/* can/mcan.c */
u16  mcan_receive(void);                                         /* 0x5E70 */
u16  mcan_send(u16 cmd, u16 handle, const u8 *data, u16 dlc);      /* 0x5EA5 */
void mcan_process(void);                                         /* 0x801A */

/* can/mcan_slave.c */
void mcan_request_advance(lok_state_t *d, u16 state);            /* 0x5E31 */
void mcan_slave_release_all(void);                               /* 0x5EF1 */
void mcan_slave_lost(void);                                      /* 0x5F19 */

/* can/mcan_name.c */
void mcan_send_device_name(void);                                /* 0x6F6B */

#endif
