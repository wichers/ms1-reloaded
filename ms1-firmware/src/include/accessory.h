/*==========================================================================
 * accessory.h - DCC accessory devices: the one in use or being edited, and the commands on
 * their way to the track (ui/accessory.c, lok/library.c); src/globals.c holds them
 *========================================================================*/
#ifndef ACCESSORY_H
#define ACCESSORY_H

#include "hcs12.h"

#define ACC_ROWS        4               /* turnouts per device: a row of keys each */
#define ACC_MAX_ADDRESS 2044            /* output addresses 1-2044 (RCN-213) */
#define ACC_CMDS        4

typedef struct {
    u8  id;                             /* its library record; 0: not saved yet */
    u8  name[16];                       /* as a locomotive's */
    u16 address;                        /* the output address of row 0; row r: address + r */
    u8  icons[8];                       /* per function key the icon variant beside it
                                         * (ui/prog.c's key table; 3 none) */
    u8  index;                          /* its place in the library's list */
    u8  state, known;                   /* bit per row: the last output switched (1 the
                                         * right key's); switched since chosen */
    u8  page, editing, cursor;          /* the editor */
} acc_device_t;

typedef struct {
    u16 address;                        /* output address; 0: free */
    u8  output;                         /* 0 or 1 */
    u8  left;                           /* activation packets still to send */
    u16 since;                          /* ms_tick of the last one: off after ACC_ON_MS */
} acc_cmd_t;

extern acc_device_t acc_dev;
extern acc_cmd_t acc_cmds[ACC_CMDS];
extern u8 acc_pending;                  /* commands in acc_cmds */

#endif /* ACCESSORY_H */
