/*==========================================================================
 * signal/mm_encode.c - MM packets for the command buffer (0xD126-0xD34E)
 *
 * An MM command buffer entry (type 9, 8 data bytes) holds the packet twice:
 *   {9, 8, 0x3C, address, function, data, 0x0F, address, function, data}
 * address: mm_trit_table[address index] (index 80 is address 0); function: 0xC0 on, 0 off;
 * data: the speed/direction or function trits. The track signal generator (build/asm/siggen.s)
 * sends the entries.
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/rom_tables.h"
#include "../include/track_cmd.h"

#define MM_FUNC(f)  (((f) & 1) ? 0xC0 : 0)

/* The direction packet: data 0x91 (bit 7 of speed clear) or 0xC5 (set). */
void mm_write_direction(u8 addr, u8 speed, u16 funcs)
{
    u8 *p = (u8 *)track_cmd_write_ptr;
    u8 d = (speed & 0x80) ? 0xC5 : 0x91;

    p[0] = 9;
    p[1] = 8;
    p[2] = 0x3C;
    p[3] = mm_trit_table[addr];
    p[4] = MM_FUNC(funcs);
    p[5] = d;
    p[6] = 0x0F;
    p[7] = mm_trit_table[addr];
    p[8] = MM_FUNC(funcs);
    p[9] = d;
    track_cmd_advance();
}

/* The MM2 packet: kind 0 speed (0-127 in 15 steps) and direction (speed bit 7), kind 1-4
 * function F1-F4 (bit kind of funcs) with the speed; kind 5-8 F5-F8 on the next address
 * (funcs >> 4) with speed 0. mm_data_table: per kind 15 data bytes per state (off/forward,
 * on/reverse). */
void mm_write_packet(u16 addr, u8 speed, u16 funcs, s16 kind)
{
    u8 *p;
    const u8 *t;
    u8 step;

    if (kind > 4) {
        speed = 0;
        if (++addr == 0x100)
            addr = 1;
        kind -= 4;
        funcs >>= 4;
    }
    step = (u8)((u16)((speed & 0x7F) * 14 + 127) >> 7);
    t = &mm_data_table[step + kind * 30];
    if (kind != 0 ? (funcs & (1 << kind)) != 0 : (speed & 0x80) != 0)
        t += 15;
    p = (u8 *)track_cmd_write_ptr;
    p[0] = 9;
    p[1] = 8;
    p[2] = 0x3C;
    p[3] = mm_trit_table[(u8)addr];
    p[4] = MM_FUNC(funcs);
    p[5] = *t;
    p[6] = 0x0F;
    p[7] = mm_trit_table[(u8)addr];
    p[8] = MM_FUNC(funcs);
    p[9] = *t;
    track_cmd_advance();
}

/* The MM1 reverse packet: speed step 1 (data 0xC0), which reverses an MM1 decoder. */
void mm_write_reverse(u8 addr, u8 func)
{
    u8 *p = (u8 *)track_cmd_write_ptr;
    u8 f = MM_FUNC(func);

    p[0] = 9;
    p[1] = 8;
    p[2] = 0x3C;
    p[3] = mm_trit_table[addr];
    p[4] = f;
    p[5] = 0xC0;
    p[6] = 0x0F;
    p[7] = mm_trit_table[addr];
    p[8] = f;
    p[9] = 0xC0;
    track_cmd_advance();
}

/* An MM idle packet: all address trits open, speed 0 (0xD112) */
void mm_write_idle(void)
{
    mm_write_packet(0, 0, 0, 0);
}
