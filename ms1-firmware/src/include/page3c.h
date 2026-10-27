/*==========================================================================
 * page3c.h - the functions the application calls in flash page $3C
 *
 * Page $3C is linked on its own (gcc/page3c.ld), after the application and against its
 * symbols. A CALL maps it into the window at 0x8000 in place of page $3D for the time of the
 * call, so code there may call and read only the fixed pages, RAM and page $3C itself;
 * tools/banks.py keeps everything it calls, and every interrupt handler, in the fixed pages.
 *
 * The application reaches page $3C only through the entry table at its start: a JMP (3 bytes)
 * per name of PAGE3C_ENTRIES, in this order (src/sys/page3c_entries.c). The application's
 * link gives each name the table entry's address in the linker's banked form, 0x100000 +
 * offset (build/page3c_symbols.ld, tools/banks.py), so GCC's CALL of a far function carries
 * page $3C. A new entry goes at the end, with its far declaration below.
 *========================================================================*/
#ifndef PAGE3C_H
#define PAGE3C_H

#include "hcs12.h"

#define PAGE3C_ENTRIES(X) \
    X(dcc_slot_job) \
    X(dcc_slot_refresh) \
    X(library_count) \
    X(library_name) \
    X(library_to_edit) \
    X(library_save) \
    X(dcc_service_start) \
    X(dcc_service_status) \
    X(dcc_service_end) \
    X(dcc_service_next) \
    X(dcc_ack_sample) \
    X(prog_offered) \
    X(prog_item) \
    X(prog_text)

/* signal/dcc.c: DCC locomotives on the track */
u16 dcc_slot_job(u16 slot) __attribute__((far));
u16 dcc_slot_refresh(u16 slot) __attribute__((far));

/* lok/library.c: the device library */
#define LIB_LOCO    1                   /* the kind of a record: a locomotive */
#define LIB_DONE    0                   /* library_save: written */
#define LIB_AGAIN   1                   /* it set STOP for the writing: call it again */
#define LIB_FULL    2
#define LIB_ERROR   3                   /* the flash reported an error */
u16 library_count(u16 kind) __attribute__((far));
u16 library_name(u16 kind, u16 index, u8 *dest) __attribute__((far));
u16 library_to_edit(u16 index) __attribute__((far));
u16 library_save(u16 slot) __attribute__((far));

/* signal/dcc_service.c: DCC service mode (include/dcc.h) */
u16 dcc_service_start(u16 job, u16 cv, u16 value) __attribute__((far));
u16 dcc_service_status(void) __attribute__((far));
u16 dcc_service_end(void) __attribute__((far));
u16 dcc_service_next(void) __attribute__((far));
u16 dcc_ack_sample(void) __attribute__((far));

/* ui/prog.c: the programming items of LOK ÄNDERN (include/ui.h) */
u16 prog_offered(u16 item, u16 slot) __attribute__((far));
u16 prog_item(u16 slot, u16 sub) __attribute__((far));
u16 prog_text(u16 slot, u8 *buf) __attribute__((far));

#endif /* PAGE3C_H */
