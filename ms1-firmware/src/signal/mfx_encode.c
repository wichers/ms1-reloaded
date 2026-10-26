/*==========================================================================
 * signal/mfx_encode.c - mfx frames (0xCA80-0xCF09)
 *
 * An mfx frame is built bit by bit in the current command buffer entry (track_cmd_write_ptr):
 * byte 0 the entry type (4 = mfx frame), byte 1 the bit count, the bits from byte 2 on,
 * MSB first; mfx_byte_idx/mfx_bit_pos is the write position. The CRC-8 (polynomial 0x07,
 * start 0xFF) runs over the bits as they are written. The track signal generator
 * (build/asm/siggen.s) turns the entries into the track signal.
 *
 * Functions:
 *   mfx_put_bits        (0xCA80) - append bits
 *   mfx_put_command         (0xCB0E) - command code
 *   mfx_cmd_syncs    (0xCB8C) - command buffer entry type 3
 *   mfx_cmd_feedback  (0xCBA1) - command buffer entry type 5
 *   mfx_frame_begin        (0xCBBF) - start a frame: sync bit, address in 7/9/11/14 bits
 *   mfx_put_estop      (0xCC31) - command with a fixed byte
 *   mfx_put_drive   (0xCC58) - speed command, 4 or 8 bits
 *   mfx_put_functions     (0xCCBD) - function command, 4, 8 or 16 bits
 *   mfx_frame_end        (0xCD0D) - append the CRC, close the entry
 *   mfx_frame_read_cv      (0xCD6F) - frame: command 9
 *   mfx_frame_write_cv   (0xCDB7) - frame: command 10 with data bytes
 *   mfx_frame_search (0xCE2B) - frame: command 11
 *   mfx_frame_assign  (0xCE5E) - frame: command 12
 *   mfx_frame_ping    (0xCE91) - frame: command 13
 *   mfx_frame_central    (0xCEB4) - frame: command 14 with a 32-bit UID
 *   mfx_frame_cmd20   (0xCEEB) - frame: command 20
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"
#include "../include/track_cmd.h"

void mfx_put_bits(u16 value, s16 nbits)
{
    u16 mask, acc;
    s16 n = nbits;
    u8 *p, bit;

    if (n <= 0)
        return;
    mask = (u16)1 << (n - 1);
    do {
        if (mfx_bit_pos > 7) {
            mfx_bit_pos = 0;
            mfx_byte_idx++;
        }
        p = (u8 *)track_cmd_write_ptr + mfx_byte_idx + 2;
        acc = (u16)*p << 1;
        bit = (value & mask) != 0;
        if (bit)
            acc++;
        *p = (u8)acc;
        if (mfx_crc_state & 0x80)
            mfx_crc_state = (u8)(mfx_crc_state << 1) ^ 7 ^ bit;
        else
            mfx_crc_state = (u8)(mfx_crc_state << 1) ^ bit;
        mfx_bit_pos++;
        mask >>= 1;
    } while (--n);
}

/* 1: 00, 2: 01, n >= 3: 1, (n - 3) / 6 times 11, (n - 3) % 6 in 3 bits */
void mfx_put_command(u16 op)
{
    s16 n, q;

    if (op == 1) {
        mfx_put_bits(0, 2);
    } else if (op == 2) {
        mfx_put_bits(1, 2);
    } else {
        mfx_put_bits(1, 1);
        n = (s16)op - 3;
        q = n / 6;
        mfx_put_bits(0xFFFF, q * 2);
        mfx_put_bits(n - q * 6, 3);
    }
}

void mfx_cmd_syncs(u16 count)
{
    if (count != 0) {
        ((u8 *)track_cmd_write_ptr)[0] = 3;
        ((u8 *)track_cmd_write_ptr)[1] = (u8)count;
        track_cmd_advance();
    }
}

void mfx_cmd_feedback(u8 a, u8 b)
{
    ((u8 *)track_cmd_write_ptr)[0] = 5;
    ((u8 *)track_cmd_write_ptr)[1] = a + 1;
    ((u8 *)track_cmd_write_ptr)[2] = b;
    track_capture_samples = 0;
    track_cmd_advance();
}

void mfx_frame_begin(u16 addr)
{
    mfx_byte_idx = 0;
    mfx_bit_pos = 0;
    mfx_put_bits(1, 1);
    mfx_crc_state = 0xFF;
    if (addr <= 0x7F) {
        mfx_put_bits(0, 1);
        mfx_put_bits(addr, 7);
    } else if (addr <= 0x1FF) {
#if MS1_RELEASE == 0x4010
        mfx_put_bits(1, 2);                     /* "101": not the mfx format's "110" */
#else
        mfx_put_bits(2, 2);
#endif
        mfx_put_bits(addr, 9);
    } else if (addr <= 0x7FF) {
#if MS1_RELEASE == 0x4010
        mfx_put_bits(3, 3);                     /* "1011": not the mfx format's "1110" */
#else
        mfx_put_bits(6, 3);
#endif
        mfx_put_bits(addr, 11);
    } else {
        mfx_put_bits(7, 3);
        mfx_put_bits(addr, 14);
    }
}

/* The original reads the byte it sends from the wrong stack offset, the high byte of its
 * return address; its one caller is at 0x473B, so the byte is (0x47 & 0x80) | 1 = 1. The
 * argument b is unused. */
void mfx_put_estop(u8 b, u8 op)
{
    (void)b;
    if (op == 0)
        return;
    mfx_put_command(op);
    mfx_put_bits(1, 1);
    mfx_put_bits(1, 8);
}

/* v: bit 7 direction, bits 0-6 speed (0, or 2-127 for 1-126) */
void mfx_put_drive(u8 v, u8 op)
{
    u8 s;

    if (op == 0)
        return;
    s = (v & 0x7F) ? (v & 0x7F) + 1 : 0;
    if ((s8)s < 0)
        s = 0x7F;
    v = (v & 0x80) | s;
    mfx_put_command(op);
    if ((v & 0x0F) == 0) {
        mfx_put_bits(0, 1);
        mfx_put_bits(v >> 4, 4);
    } else {
        mfx_put_bits(1, 1);
        mfx_put_bits(v, 8);
    }
}

void mfx_put_functions(u16 v, u8 op)
{
    if (op == 0)
        return;
    mfx_put_command(op);
    if (v < 0x10) {
        mfx_put_bits(0, 1);
        mfx_put_bits(v, 4);
    } else if (v < 0x100) {
        mfx_put_bits(2, 2);
        mfx_put_bits(v, 8);
    } else {
        mfx_put_bits(3, 2);
        mfx_put_bits(v, 16);
    }
}

void mfx_frame_end(void)
{
    u8 crc = mfx_crc_state;
    u8 i;
    u8 *p;

    for (i = 8; i != 0; i--)
        crc = (crc & 0x80) ? (u8)(crc << 1) ^ 7 : (u8)(crc << 1);
    mfx_crc_state = crc;
    mfx_put_bits(crc, 8);
    p = (u8 *)track_cmd_write_ptr;
    p[0] = 4;
    p[1] = (u8)(mfx_bit_pos << 4) + mfx_byte_idx + 1;
    p += mfx_byte_idx + 2;
    *p = (u8)((u16)*p << (8 - mfx_bit_pos));
    track_cmd_advance();
}

/* the 2-bit field of commands 9 and 10 from n: 0 for n <= 1, 1 for 2, 2 for 3-4, else 3 */
static u16 count_field(s16 n)
{
    u16 x = 3;

    if (n <= 4)
        x = 2;
    if (n <= 2)
        x = 1;
    if (n <= 1)
        x = 0;
    return x;
}

void mfx_frame_read_cv(u16 addr, u16 a2, s16 n)
{
    mfx_frame_begin(addr);
    mfx_put_command(9);
    mfx_put_bits(a2, 16);
    mfx_put_bits(count_field(n), 2);
    mfx_frame_end();
}

void mfx_frame_write_cv(u16 addr, u16 a2, s16 count, const u8 *data)
{
    s16 i;

    mfx_frame_begin(addr);
    mfx_put_command(10);
    mfx_put_bits(a2, 16);
    mfx_put_bits(count_field(count), 2);
    for (i = 0; i < count; i++)
        mfx_put_bits(data[i], 8);
    mfx_frame_end();
}

void mfx_frame_search(u16 x, u16 a2, u16 a3)
{
    mfx_frame_begin(0);
    mfx_put_command(11);
    mfx_put_bits(x, 6);
    mfx_put_bits(a2, 16);
    mfx_put_bits(a3, 16);
    mfx_frame_end();
}

void mfx_frame_assign(u16 x, u16 a2, u16 a3)
{
    mfx_frame_begin(0);
    mfx_put_command(12);
    mfx_put_bits(x, 14);
    mfx_put_bits(a2, 16);
    mfx_put_bits(a3, 16);
    mfx_frame_end();
}

void mfx_frame_ping(u16 addr, u16 a2, u16 a3)
{
    mfx_frame_begin(addr);
    mfx_put_command(13);
    mfx_put_bits(a2, 16);
    mfx_put_bits(a3, 16);
    mfx_frame_end();
}

void mfx_frame_central(u32 uid, u16 a2)
{
    mfx_frame_begin(0);
    mfx_put_command(14);
    mfx_put_bits((u16)(uid >> 16), 16);
    mfx_put_bits((u16)uid, 16);
    mfx_put_bits(a2, 16);
    mfx_frame_end();
}

void mfx_frame_cmd20(u16 x)
{
    mfx_frame_begin(0);
    mfx_put_command(0x14);
    mfx_put_bits(x, 16);
    mfx_frame_end();
}
