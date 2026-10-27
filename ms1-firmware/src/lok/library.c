/*==========================================================================
 * lok/library.c - the device library: the user's locomotives and accessory devices, in flash
 * page $3C (include/page3c.h)
 *
 * The top LIB_BLOCKS 512-byte blocks of page $3C, from LIB_START (gcc/page3c.ld leaves them
 * out of the link), are a log of 64-byte rows, one record each (lib_row_t):
 *   kind      LIB_LOCO, LIB_ACCESSORY, or LIB_DELETED for a deleted device; 0xFF a free row
 *   id        the device, 1-254; a locomotive is found by protocol and address, an accessory
 *             device by its id (acc_dev.id)
 *   seq       rising with each row written: of a device's rows the newest counts
 *   ...       a locomotive's name, protocol, symbol, flags, address, functions (icon and key);
 *             an accessory device's name, its first output address, per function key the
 *             icon beside it (functions[k][1])
 *   check     the XOR of the bytes before it
 *   correction a word that makes the row change the bootloader's CRC over page $3C exactly as
 *             an erased row does (docs/bootloader.md: Reset), so the application stays
 *             startable whatever the library holds: run the CRC (0x1021, the bootloader's
 *             form) from 0 over the bytes before it XOR 0xFF, then two zero bytes, and take the
 *             result XOR 0xFFFF
 * Rows are written in ring order, block after block, each with the next sequence number. One
 * block is always erased: before the writing moves into a block, the block after it - the
 * oldest - is collected: its rows that are their device's newest go to the head again with
 * new numbers, then it is erased. So a walk backwards through the ring from the newest row
 * meets every device's newest row first (live_scan). At most LIB_MAX devices.
 *
 * Writing: the flash cannot be read while a command runs, so flash_run copies the command
 * routine to RAM (flash_ram) and runs it with interrupts masked: about 1 ms per row, 20-27 ms
 * per block erase. The track signal waits meanwhile, so writing needs STOP: library_save sets
 * it and asks to be called again, and clears it when done if it set it. A power loss while a
 * row is written leaves the bootloader's CRC wrong: the MS1 then starts in update mode, and a
 * firmware update, which erases the library, brings it back.
 *
 * Functions:
 *   library_count             - the number of devices of a kind
 *   library_name              - the name of one, newest first
 *   library_to_edit           - a locomotive as the locomotive being added (lok_edit)
 *   library_save              - a slot's locomotive into the library
 *   library_load_accessory    - an accessory device into acc_dev
 *   library_save_accessory    - acc_dev into the library
 *   library_delete_accessory  - acc_dev out of it
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/asm.h"
#include "../include/globals.h"
#include "../include/lokstack.h"
#include "../include/page3c.h"
#include "../include/accessory.h"

#define LIB_START   0xB000              /* page $3C; gcc/page3c.ld ends the code below it */
#define ROW_SIZE    64
#define BLOCK_ROWS  8
#define LIB_ROWS    64
#define LIB_BLOCKS  (LIB_ROWS / BLOCK_ROWS)
#define LIB_MAX     ((LIB_BLOCKS - 2) * BLOCK_ROWS)
#define NO_ROW      0xFF

#define LIB_DELETED 0
#define LIB_FREE    0xFF

#define FCLK_DIV    20                  /* 4 MHz / 21: 190 kHz, within 150-200 kHz */
#define CMD_PROGRAM 0x20
#define CMD_ERASE   0x40
#define FSTAT_ERR   0x30                /* PVIOL, ACCERR */

typedef struct {
    u8  kind;
    u8  id;
    u16 seq;
    u8  name[16];
    u8  protocol;
    u8  symbol;
    u16 protocol_flags;
    u16 address;
    u8  functions[16][2];               /* icon, key (lok_function_t) */
    u8  reserved[3];
    u8  check;
    u16 correction;
} lib_row_t;

typedef char lib_row_size_check[sizeof(lib_row_t) == ROW_SIZE ? 1 : -1];

#define ROW(r)      ((const lib_row_t *)(LIB_START + (u16)(r) * ROW_SIZE))

/* ---- the flash commands, from RAM ---- */

/* The routine flash_run copies to flash_ram: A the command, B the words, X the flash address,
 * Y the data; per word it waits for the command buffer, writes the word to the array, starts
 * the command, and stops at an error; then it waits until all commands are done and returns
 * FSTAT in B. Relative branches only: it runs at another address. */
__asm__ (
"	.sect	.rodata.flash_routine\n"
"	.globl	flash_routine, flash_routine_end\n"
"flash_routine:\n"
"1:	brclr	FSTAT,#0x80,1b\n"
"	movw	2,y+,2,x+\n"
"	staa	FCMD\n"
"	movb	#0x80,FSTAT\n"
"	brset	FSTAT,#0x10,2f\n"                 /* ACCERR */
"	brset	FSTAT,#0x20,2f\n"                 /* PVIOL */
"	dbne	b,1b\n"
"2:	brclr	FSTAT,#0x40,2b\n"
"	ldab	FSTAT\n"
"	rts\n"
"flash_routine_end:\n"
);

extern const u8 flash_routine[], flash_routine_end[];     /* gcc/page3c.ld checks its size */

/* one flash command over words words from data at addr (page $3C's window); the error bits */
static u8 flash_run(u8 cmd, u8 words, u16 addr, const void *data)
{
    u16 d = (u16)cmd << 8 | words, ccr;
    const u8 *s;
    u8 *t;

    for (s = flash_routine, t = flash_ram; s != flash_routine_end; )
        *t++ = *s++;
    if (!(FCLKDIV & 0x80))
        FCLKDIV = FCLK_DIV;
    FSTAT = FSTAT_ERR;                  /* clear old errors */
    IRQ_SAVE(ccr);
    __asm__ __volatile__ ("jsr\tflash_ram" : "+d" (d), "+x" (addr), "+y" (data) : : "memory");
    IRQ_RESTORE(ccr);
    return (u8)d & FSTAT_ERR;
}

/* the bootloader's CRC step (tools/mkimage.py crc_update) */
static u16 crc_byte(u16 crc, u8 b)
{
    u8 n;

    for (n = 8; n; n--, b <<= 1) {
        crc = (crc & 0x8000) ? (u16)(crc << 1) ^ 0x1021 : (u16)(crc << 1);
        if (b & 0x80)
            crc ^= 1;
    }
    return crc;
}

static u8 write_row(u8 r, lib_row_t *row)
{
    const u8 *p = (const u8 *)row;
    u16 c = 0;
    u8 x = 0, i;

    for (i = 0; i < ROW_SIZE - 3; i++)
        x ^= p[i];
    row->check = x;
    for (i = 0; i < ROW_SIZE - 2; i++)
        c = crc_byte(c, p[i] ^ 0xFF);
    c = crc_byte(crc_byte(c, 0), 0);
    row->correction = c ^ 0xFFFF;
    return flash_run(CMD_PROGRAM, ROW_SIZE / 2, (u16)ROW(r), row);
}

/* erase block b, unless it is erased */
static u8 erase_block(u8 b)
{
    static const u16 any = 0xFFFF;
    const u16 *p = (const u16 *)ROW(b * BLOCK_ROWS);
    u16 n;

    for (n = BLOCK_ROWS * ROW_SIZE / 2; n && *p == 0xFFFF; n--, p++)
        ;
    return n ? flash_run(CMD_ERASE, 1, (u16)ROW(b * BLOCK_ROWS), &any) : 0;
}

/* ---- the log ---- */

/* the newest row, or NO_ROW when the library is empty */
static u8 head(void)
{
    u8 r, best = NO_ROW;

    for (r = 0; r < LIB_ROWS; r++)
        if (ROW(r)->kind != LIB_FREE && (best == NO_ROW || (s16)(ROW(r)->seq - ROW(best)->seq) > 0))
            best = r;
    return best;
}

/* Walk from the newest row back: each device's newest row comes first. Calls of
 * live_next return the live ones (not deleted) of kind kind, newest first; NO_ROW at the end.
 * seen marks the ids met, 32 bytes. */
typedef struct {
    u8 r, left, kind;
    u8 seen[32];
} walk_t;

static void walk_start(walk_t *w, u8 kind)
{
    u8 i;

    for (i = 0; i < 32; i++)
        w->seen[i] = 0;
    w->r = head();
    w->left = w->r == NO_ROW ? 0 : LIB_ROWS;
    w->kind = kind;
}

/* the next device's newest row (any kind, deleted too), or NO_ROW */
static u8 walk_device(walk_t *w)
{
    const lib_row_t *row;
    u8 r, bit;

    while (w->left) {
        r = w->r;
        w->r = r ? r - 1 : LIB_ROWS - 1;
        w->left--;
        row = ROW(r);
        if (row->kind == LIB_FREE)
            continue;
        bit = 1 << (row->id & 7);
        if (w->seen[row->id >> 3] & bit)
            continue;
        w->seen[row->id >> 3] |= bit;
        return r;
    }
    return NO_ROW;
}

static u8 live_next(walk_t *w)
{
    u8 r;

    while ((r = walk_device(w)) != NO_ROW)
        if (ROW(r)->kind == w->kind)
            return r;
    return NO_ROW;
}

/* the index-th live row of kind, newest first, or NO_ROW */
static u8 live_row(u8 kind, u16 index)
{
    walk_t w;
    u8 r;

    walk_start(&w, kind);
    while ((r = live_next(&w)) != NO_ROW && index)
        index--;
    return r;
}

/* Room for one more row: the row after the newest, collecting the oldest block when the
 * writing moves into a new block. The row to write, or NO_ROW (a flash error, or the library
 * full of live rows). seq: the next sequence number. */
static u8 next_free(u16 *seq)
{
    lib_row_t copy;
    walk_t w;
    const u8 *s;
    u8 *t;
    u8 h = head(), r, old, i, n, start;

    if (h == NO_ROW) {
        *seq = 1;
        return 0;
    }
    *seq = ROW(h)->seq + 1;
    r = (u8)((h + 1) % LIB_ROWS);
    for (n = LIB_BLOCKS; n; n--) {
        if (r % BLOCK_ROWS != 0)
            return r;
        /* r begins a block: the block after it, the oldest, goes - its devices' newest rows
         * first to this one */
        start = r;
        old = (u8)((r / BLOCK_ROWS + 1) % LIB_BLOCKS);
        walk_start(&w, 0);
        while ((i = walk_device(&w)) != NO_ROW) {
            if (i / BLOCK_ROWS != old || ROW(i)->kind == LIB_DELETED)
                continue;
            for (s = (const u8 *)ROW(i), t = (u8 *)&copy; t != (u8 *)&copy + ROW_SIZE; )
                *t++ = *s++;
            copy.seq = (*seq)++;
            if (write_row(r, &copy))
                return NO_ROW;
            r++;
        }
        if (erase_block(old))
            return NO_ROW;
        if (r != start + BLOCK_ROWS)
            return r;
        r %= LIB_ROWS;                  /* the block is full of copies: the next one */
    }
    return NO_ROW;
}

/* ---- the entries ---- */

u16 library_count(u16 kind)
{
    walk_t w;
    u16 n = 0;

    walk_start(&w, (u8)kind);
    while (live_next(&w) != NO_ROW)
        n++;
    return n;
}

u16 library_name(u16 kind, u16 index, u8 *dest)
{
    u8 r = live_row((u8)kind, index), i;

    for (i = 0; i < 16; i++)
        dest[i] = r == NO_ROW ? 0 : ROW(r)->name[i];
    return r != NO_ROW;
}

/* lok_edit from the index-th locomotive, as a new locomotive from the database: its symbol,
 * protocol, address, name and functions, the default parameters */
u16 library_to_edit(u16 index)
{
    u8 r = live_row(LIB_LOCO, index), k;
    const lib_row_t *row;

    if (r == NO_ROW)
        return 0;
    row = ROW(r);
    for (k = 0; k < 16; k++)
        lok_edit.name[k] = row->name[k];
    lok_edit.name[15] = 0;
    lok_edit.symbol = row->symbol;
    lok_edit.kinds[0] = 0x80;
    lok_edit.kinds[1] = 0x82;
    lok_edit.kinds[2] = 0;
    lok_edit.protocol = row->protocol;
    lok_edit.protocol_flags = row->protocol_flags;
    lok_edit.address = row->address;
    lok_edit.default_address = 3;
    lok_edit.mfx_uid_hi = 0;
    lok_edit.mfx_uid_lo = 0;
    lok_edit.speed_object = 1;
    for (k = 0; k < 16; k++) {
        lok_edit.functions[k].icon = row->functions[k][0];
        lok_edit.functions[k].key = row->functions[k][1];
    }
    lok_assign_functions(LOK_EDIT);
    lok_edit.vmax = 0xFF;
    lok_edit.accel = 4;
    lok_edit.decel = 3;
    lok_edit.volume = 0xFF;
    lok_edit.flags = 0;
    lok_edit.unused = 0;
    return 1;
}

/* Writing needs STOP (the track signal waits for the flash): set it and ask to be called
 * again; library_stop remembers to clear it */
static u8 need_stop(void)
{
    if (system_status_flags & 1)
        return 0;
    system_status_flags |= 1;
    mcan_status_pending |= 1;
    library_stop = 1;
    return 1;
}

/* row into the library, its kind, record and id filled in (0: a new device, which gets the
 * lowest free id): LIB_DONE, LIB_FULL, LIB_ERROR; STOP back off if need_stop set it */
static u8 store(lib_row_t *row)
{
    walk_t w;
    u16 seq;
    u8 r, k, used = 0;

    for (k = 0; k < sizeof row->reserved; k++)
        row->reserved[k] = 0xFF;
    walk_start(&w, 0);
    while ((r = walk_device(&w)) != NO_ROW)
        if (ROW(r)->kind != LIB_DELETED)
            used++;
    if (row->id == 0) {
        if (used >= LIB_MAX)
            return LIB_FULL;
        for (k = 1; w.seen[k >> 3] & (1 << (k & 7)); k++)
            ;
        row->id = k;
    }
    r = next_free(&seq);
    row->seq = seq;
    k = r == NO_ROW || write_row(r, row) ? LIB_ERROR : LIB_DONE;
    if (library_stop) {
        library_stop = 0;
        system_status_flags &= ~1;
        mcan_status_pending |= 1;
    }
    return k;
}

/* The slot's locomotive (MM or DCC) into the library: its record found by protocol and address
 * is replaced, else a new one added. LIB_DONE, LIB_AGAIN (STOP set for the writing: call
 * again), LIB_FULL, LIB_ERROR (flash). */
u16 library_save(u16 slot)
{
    const lok_record_t *e = &lok_records[slot];
    lib_row_t row;
    walk_t w;
    u8 r, k;

    if (need_stop())
        return LIB_AGAIN;
    row.id = 0;
    walk_start(&w, LIB_LOCO);
    while ((r = live_next(&w)) != NO_ROW)
        if (ROW(r)->protocol == e->protocol && ROW(r)->address == e->address)
            row.id = ROW(r)->id;
    row.kind = LIB_LOCO;
    for (k = 0; k < 16; k++)
        row.name[k] = e->name[k];
    row.protocol = e->protocol;
    row.symbol = e->symbol;
    row.protocol_flags = e->protocol_flags;
    row.address = e->address;
    for (k = 0; k < 16; k++) {
        row.functions[k][0] = e->functions[k].icon;
        row.functions[k][1] = e->functions[k].key;
    }
    return store(&row);
}

/* the index-th accessory device into acc_dev; 0 when there is none */
u16 library_load_accessory(u16 index)
{
    u8 r = live_row(LIB_ACCESSORY, index), k;
    const lib_row_t *row;

    if (r == NO_ROW)
        return 0;
    row = ROW(r);
    acc_dev.id = row->id;
    for (k = 0; k < 16; k++)
        acc_dev.name[k] = row->name[k];
    acc_dev.address = row->address;
    for (k = 0; k < 8; k++)
        acc_dev.icons[k] = row->functions[k][1];
    acc_dev.index = (u8)index;
    acc_dev.state = acc_dev.known = 0;
    return 1;
}

/* acc_dev into the library (it keeps its record, or gets one): as library_save */
u16 library_save_accessory(void)
{
    lib_row_t row;
    u8 k;

    if (need_stop())
        return LIB_AGAIN;
    row.kind = LIB_ACCESSORY;
    row.id = acc_dev.id;
    for (k = 0; k < 16; k++)
        row.name[k] = acc_dev.name[k];
    row.protocol = LOK_DCC;
    row.symbol = 0;
    row.protocol_flags = 0;
    row.address = acc_dev.address;
    for (k = 0; k < 16; k++) {
        row.functions[k][0] = 0;
        row.functions[k][1] = k < 8 ? acc_dev.icons[k] : 0xFF;
    }
    k = store(&row);
    if (k == LIB_DONE)
        acc_dev.id = row.id;
    return k;
}

/* acc_dev out of the library: a row that marks its record deleted */
u16 library_delete_accessory(void)
{
    lib_row_t row;
    u8 k;

    if (acc_dev.id == 0)
        return LIB_DONE;
    if (need_stop())
        return LIB_AGAIN;
    for (k = 0; k < sizeof row; k++)
        ((u8 *)&row)[k] = 0xFF;
    row.kind = LIB_DELETED;
    row.id = acc_dev.id;
    k = store(&row);
    if (k == LIB_DONE)
        acc_dev.id = 0;
    return k;
}
