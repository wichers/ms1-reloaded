#ifndef TRACK_CMD_H
#define TRACK_CMD_H

#include "hcs12.h"

/*==========================================================================
 * The track command buffer (track_cmd_buf, 0x1268): entries the track signal generator
 * (signal/signal_gen.c) turns into the track signal. track_cmd_write_ptr is the entry being
 * written, track_cmd_free the room left.
 *========================================================================*/

/* the generator (signal/signal_gen.c) */
void signal_gen_init(void);                                             /* 0xC58E */
void signal_gen_send_buf_executor(u16 count);                           /* 0xC614 */
void signal_gen_cmd_buf_consumer(u16 count);                            /* 0xC81D */

/* entries (signal/signal_gen.c) */
void track_cmd_advance(void);                     /* 0xCA6B: close the entry */
void track_cmd_write_type2(u8 data);                     /* 0xCA5E: entry type 2, one byte */
void track_cmd_pause(u16 timing_value);       /* 0xCA46: a pause (entry type 1) */

/* mfx frames (signal/mfx_encode.c) */
void mfx_put_bits(u16 value, s16 nbits);                                /* 0xCA80 */
void mfx_put_command(u16 op);                                           /* 0xCB0E */
void mfx_cmd_syncs(u16 count);                                          /* 0xCB8C */
void mfx_cmd_feedback(u8 a, u8 b);                                      /* 0xCBA1 */
void mfx_frame_begin(u16 addr);                                         /* 0xCBBF */
void mfx_put_estop(u8 b, u8 op);                                        /* 0xCC31 */
void mfx_put_drive(u8 v, u8 op);                                        /* 0xCC58 */
void mfx_put_functions(u16 v, u8 op);                                   /* 0xCCBD */
void mfx_frame_end(void);                                               /* 0xCD0D */
void mfx_frame_read_cv(u16 addr, u16 a2, s16 n);                        /* 0xCD6F */
void mfx_frame_write_cv(u16 addr, u16 a2, s16 count, const u8 *data);   /* 0xCDB7 */
void mfx_frame_search(u16 x, u16 a2, u16 a3);                           /* 0xCE2B */
void mfx_frame_assign(u16 x, u16 a2, u16 a3);                           /* 0xCE5E */
void mfx_frame_ping(u16 addr, u16 a2, u16 a3);                          /* 0xCE91 */
void mfx_frame_central(u32 uid, u16 a2);                                /* 0xCEB4 */
void mfx_frame_cmd20(u16 x);                                            /* 0xCEEB */

/* MM packets (signal/mm_encode.c) */
void mm_write_direction(u8 addr, u8 speed, u16 funcs);                  /* 0xD126 */
void mm_write_packet(u16 addr, u8 speed, u16 funcs, s16 kind);          /* 0xD1B4 */
void mm_write_reverse(u8 addr, u8 func);                                /* 0xD2D7 */
void mm_write_idle(void);                                               /* 0xD112 */

/* the feedback window (signal/mfx_feedback.c) */
s16  mfx_feedback_bit(u16 count);                                       /* 0xCF0A */
s16  mfx_feedback_data(u8 *decode_buf, u16 data_count);                /* 0xCFE4 */

/* the filler when no locomotive needs a packet (signal/track_filler.c) */
void track_filler(void);                                                /* 0x5952 */

#endif /* TRACK_CMD_H */
