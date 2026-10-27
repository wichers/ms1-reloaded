/*==========================================================================
 * dcc.h - DCC service mode: the state the main loop and page $3C share (signal/dcc_service.c)
 *========================================================================*/
#ifndef DCC_H
#define DCC_H

#include "hcs12.h"

#define DCC_PREAMBLE    16              /* bits, in operation */
#define SVC_PREAMBLE    22              /* bits, in service mode (RCN-216: at least 20) */
#define SVC_OPS         9               /* a CV read: 8 bit verifies and a byte verify */

/* dcc_service_start jobs */
#define SVC_READ        1               /* read CV cv: bit by bit, then the byte verified */
#define SVC_WRITE       2               /* write value to CV cv */
#define SVC_ADDRESS     3               /* the address cv, long (value 1) or short: CV1 or
                                         * CV17/CV18, and CV29 bit 5 */

/* dcc_service_status: low byte */
#define SVC_BUSY        0
#define SVC_OK          1               /* every packet acknowledged */
#define SVC_NO_ACK      2
#define SVC_ABORTED     3               /* too much current: STOP */

typedef struct {
    u8  state;          /* 0: off; nonzero: service mode, track_scheduler hands it the track */
    u8  phase;          /* the packets being sent (dcc_service.c) */
    u8  count;          /* packets left of the phase */
    u8  op, nops;       /* the operation sent, how many there are */
    u16 cv[SVC_OPS];
    u8  data[SVC_OPS];  /* the instruction's data byte */
    u8  kind[SVC_OPS];  /* its CC bits: 1 verify, 3 write, 2 bit (data 111KDBBB) */
    u16 acks;           /* bit per operation: acknowledged */
    u8  job, value;     /* the job; a read's value */
    u8  status;         /* SVC_BUSY ... */
    u8  stop;           /* STOP was on when service mode began: on again at its end */
    u8  auto_end;       /* end service mode with the job, and report a failure */
    u16 t_last;         /* ms_tick when the last instruction packet was written */
    u8  ack;            /* the sampler saw an acknowledgement since the instructions began */
    u8  above, over;    /* samples in a row above baseline + ACK_MIN_MA, above the limit */
    u16 baseline;       /* the current without an acknowledgement, x16 */
} dcc_service_t;

extern dcc_service_t dcc_svc;           /* src/globals.c */

/* page $3C, between its files */
void dcc_write_entry(const u8 *bytes, u8 n, u8 preamble);

#endif /* DCC_H */
