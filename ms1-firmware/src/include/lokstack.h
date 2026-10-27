/*==========================================================================
 * The locomotive slots: per slot a record (what is stored about the locomotive, 0x60 bytes
 * from 0x13D7) and a state (what changes while it runs, 0x1C bytes from 0x17F7).
 * lok_records[LOK_EDIT], after the ten slots, is the locomotive being added: the menus fill
 * it, then mcan_lokstack_sync copies it into a free slot.
 *
 * Word fields are big-endian, like every 16-bit value on the HCS12. Where the original sets
 * or tests one byte of a word, the code here does too (HI8/LO8), so that both compile to
 * the same accesses.
 *========================================================================*/
#ifndef LOKSTACK_H
#define LOKSTACK_H

#include "hcs12.h"

#define LOK_SLOTS   10
#define LOK_EDIT    LOK_SLOTS          /* index of the record being added */

/* protocols (lok_record_t.protocol) */
#define LOK_MFX     1
#define LOK_DCC     2
#define LOK_MM      3

/* lok_record_t.protocol_flags */
#define LOKP_MM1_REVERSE   0x0001      /* MM: direction change by the MM1 reverse packet */
#define LOKP_MM2           0x0002      /* MM: MM2 direction and functions f1-f4 */
#define LOKP_F5_F8         0x0004      /* MM: functions f5-f8 (on the next address) */
#define LOKP_PROGRAMMABLE  0x0010      /* MM: the decoder takes programming (address, CVs) */
#define LOKP_VOLUME        0x1000      /* a volume can be set */
#define LOKP_DCC_STEPS     0x0003      /* DCC: the speed steps, 0 128 (signal/dcc.c) */
#define LOKP_DCC_28        0x0001      /* DCC: 28 speed steps */
#define LOKP_DCC_14        0x0002      /* DCC: 14 speed steps */
#define LOKP_DCC_LONG      0x0004      /* DCC: the long address form, also below 128 */

/* the bytes of a word field */
#define HI8(w)  (((u8 *)&(w))[0])
#define LO8(w)  (((u8 *)&(w))[1])

/* a function of a locomotive */
typedef struct {
    u8  icon;               /* symbol, bit 7: momentary; 0: no function */
    u8  key;                /* bits 4-7: the key it is on (PORTA bit, 8 = light) */
    u8  object;             /* its CAN object number (after the speed object) */
} lok_function_t;

typedef struct {
    u8  name[16];           /* 0x00 0-terminated; [15] is always 0 */
    u8  symbol;             /* 0x10 the locomotive symbol: 0 none, 1 electric, 2 diesel, 3 steam */
    u8  protocol;           /* 0x11 LOK_MFX, LOK_DCC, LOK_MM */
    u16 protocol_flags;     /* 0x12 LOKP_* */
    u16 address;            /* 0x14 MM or DCC address; mfx: the track address (SID) */
    u16 mfx_uid_hi;         /* 0x16 mfx: decoder UID */
    u16 mfx_uid_lo;         /* 0x18 */
    u8  kinds[3];           /* 0x1A the CAN object kinds the slot answers as, 0-terminated
                             *      (0x80 locomotive, 0x82 ...) */
    u8  mfx_drive_cmd;      /* 0x1D mfx: command code of drive frames (0: none) */
    u8  mfx_function_cmd;   /* 0x1E mfx: command code of function frames (0: none) */
    u8  speed_object;       /* 0x1F CAN object number of its speed; 0: cannot be driven */
    u8  vmax;               /* 0x20 maximum speed */
    u8  accel;              /* 0x21 acceleration */
    u8  decel;              /* 0x22 braking */
    u8  volume;             /* 0x23 */
    lok_function_t functions[16];   /* 0x24 F0-F15 */
    u16 mfx_name_cv;        /* 0x54 mfx: CV (number << 6 | index) of the name */
    u16 mfx_volume_cv;      /* 0x56 mfx: CA (number << 6) of the volume */
    union {
        u16 default_address;    /* 0x58 MM, DCC: the decoder's address after a reset */
        u16 mfx_vmax_cv;        /* 0x58 mfx: CA of the speed curve (Vmax at index 2) */
    };
    u16 mfx_accel_cv;       /* 0x5A mfx: CA of acceleration (index 1) and braking (2) */
    u8  mfx_write_syncs;    /* 0x5C mfx: sync patterns after a CV write */
    u8  mfx_read_width;     /* 0x5D mfx: bytes per CV read, 1 or 2 */
    u8  flags;              /* 0x5E bit 0: its address conflicts with another slot's */
    u8  unused;             /* 0x5F nonzero: the slot holds no locomotive */
} lok_record_t;

/* lok_state_t.flags */
#define LOKS_DELETED    0x80        /* being removed */
#define LOKS_NO_TRACK   0x40        /* not (yet) sent on the track */
#define LOKS_NO_ANSWER  0x20        /* mfx: the decoder does not answer pings */
#define LOKS_DRIVEN     0x02        /* driven on this MS1 (lok_take_current) */

typedef struct {
    u16 object;             /* 0x00 CAN object id of the slot (0x200 + slot) */
    u8  speed;              /* 0x02 bits 0-6 speed (0-127), bit 7 reverse */
    u16 functions;          /* 0x03 F0-F15, a bit each */
    u8  flags;              /* 0x05 LOKS_*, bits 0-2 control by CAN devices */
    u16 controller;         /* 0x06 CAN address of the device driving it; 0: none */
    u8  timer;              /* 0x08 countdown of the pending CAN exchange; mfx: unanswered pings */
    u16 prog_cv;            /* 0x09 a programming request (MM, DCC): CV; 0: none */
    u8  prog_value;         /* 0x0B its value */
    u16 save_pending;       /* 0x0C changes to store in the EEPROM */
    u16 track_pending;      /* 0x0E changes to send on the track: bit 0 speed, 1 direction,
                             *      2-8 programming items, 14 reset, 12 slot removed */
    u16 track_functions;    /* 0x10 functions changed, to send on the track */
    u16 can_pending;        /* 0x12 changes to send on CAN */
    u16 can_functions;      /* 0x14 functions changed, to send on CAN */
    u8  mm_phase;           /* 0x16 MM refresh: which packet comes next */
    u16 change_count;       /* 0x17 counts the slot's changes (never 0) */
    u8  name_sent;          /* 0x19 position in the name being sent on CAN */
    u16 request;            /* 0x1A slave: the next request to the master */
} lok_state_t;

/* offset of a field in a record (as in the EEPROM copy of the slots) */
#define LOK_RECORD_OFFSET(f)    ((u16)&((lok_record_t *)0)->f)

/* the layout is the original's: a compile error here means a field moved */
typedef char lok_record_size_check[sizeof(lok_record_t) == 0x60 ? 1 : -1];
typedef char lok_state_size_check[sizeof(lok_state_t) == 0x1C ? 1 : -1];
typedef char lok_record_offset_check[(unsigned)&((lok_record_t *)0)->mfx_name_cv == 0x54 ? 1 : -1];
typedef char lok_state_offset_check[(unsigned)&((lok_state_t *)0)->change_count == 0x17 ? 1 : -1];

extern lok_record_t lok_records[LOK_SLOTS + 1];   /* 0x13D7 */
extern lok_state_t  lok_states[LOK_SLOTS];        /* 0x17F7 */
#define lok_edit    (lok_records[LOK_EDIT])

/* lok/lokstack.c: slots, and the CAN objects (mcan_*: by object handle) */
s16  lok_slot_of_object(u16 hash);                                  /* 0x59FB */
u16  mcan_object_kind(u16 id);                                       /* 0x5A37 */
u16  mcan_kind_class(u8 kind);                                   /* 0x5AC1 */
u16  mcan_object_class(u16 id);                                      /* 0x5AF4 */
u16  mcan_object_change_count(u16 id);                               /* 0x5AFA */
u16  mcan_object_controller(u16 id);                                 /* 0x5B42 */
u16  mcan_next_object(u16 id, u8 kind, s8 step);                     /* 0x5B85 */
void lok_state_clear(u16 slot);                                     /* 0x5D76 */
u16  lok_function_of_object(const lok_record_t *e, u8 object);      /* 0x713B */
u16  lok_highest_object(const lok_record_t *e);                     /* 0x7160 */
s16  lok_find_free_slot(void);                                      /* 0x7185: LOK_SLOTS when none */
void lok_take_current(void);                                        /* 0xD898 */
void lok_release_current(void);                                     /* 0xD8B6 */
s16  lok_next_used_slot(s8 slot, s8 step);                          /* 0xD963 */
void lok_make_name_unique(u16 slot);                                /* 0xDA51 */

/* address conflicts between slots (lok/lokstack.c) */
s16  lok_find_address(u16 addr, u8 proto, s8 skip, s8 n);          /* 0xD8D1 */
void lok_recheck_conflicts(void);                                   /* 0xD9CD */
void lok_check_conflict(u16 slot);                                  /* 0x5DB6 */

/* lok/lok_functions.c */
void lok_assign_functions(u16 slot);                                /* 0x5C12 */

#endif /* LOKSTACK_H */
