/*==========================================================================
 * signal/mfx_feedback.c - evaluating a captured mfx feedback window
 *
 * In a feedback window the decoder answers through the receiver (RDS demodulator): the
 * track signal generator (build/asm/siggen.s) starts a capture before the window and stops it
 * after; track_capture_samples is then nonzero when something was captured. A 1-bit window
 * captures the carrier level (ATD0 channel 2) into timer_capture_buffer; a data window
 * captures, per receiver clock, the data bit and the quality bit, packed MSB first from
 * timer_capture_buffer + 5.
 *
 * Functions:
 *   mfx_feedback_bit       (0xCF0A) - did a decoder answer in a 1-bit window
 *   mfx_feedback_read_bit  (0xCFA7) - the next captured bit
 *   mfx_feedback_data      (0xCFE4) - the bytes of a data window
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/globals.h"

/*--------------------------------------------------------------------------
 * mfx_feedback_bit - Did a decoder send the feedback carrier? (0xCF0A)
 *
 * The capture of a 1-bit feedback window holds ATD0 channel 2 (the carrier level) at each
 * edge of the receiver's clock: count samples during the first pause, when an answering
 * decoder sends the carrier, then count during the second pause, when none does. The first
 * pause's samples 2..count-3 minus the second's count+3..2*count-2, the largest sample of each
 * left out, must exceed 30 per sample. Signed 16-bit arithmetic, as the original.
 *
 * Returns 0 when nothing was captured, 1 for an answer, -1 for none.
 *------------------------------------------------------------------------*/
s16 mfx_feedback_bit(u16 count)
{
    const u8 *buf = timer_capture_buffer;
    s16 i, v, sum = 0, max_val = 0;

    if (track_capture_samples == 0)
        return 0;
    for (i = 2; i < (s16)(count - 2); i++) {
        v = buf[i];
        if (v > max_val)
            max_val = v;
        sum += v;
    }
    sum -= max_val;
    max_val = 0;
    for (i = (s16)(count + 3); i < (s16)(2 * count - 1); i++) {
        v = buf[i];
        if (v > max_val)
            max_val = v;
        sum -= v;
    }
    sum += max_val;
    if ((s16)(count * 30) >= sum)
        return -1;
    return 1;
}

/*--------------------------------------------------------------------------
 * mfx_feedback_read_bit - The next captured bit, MSB first (0xCFA7)
 *------------------------------------------------------------------------*/
u16 mfx_feedback_read_bit(void)
{
    u16 result = 0;

    if (mfx_decode_bits_remaining == 0) {
        u8 *ptr = (u8 *)mfx_decode_read_ptr;
        mfx_decode_current_byte = *ptr++;
        mfx_decode_read_ptr = (u16)ptr;
        mfx_decode_bits_remaining = 8;
    }
    if (mfx_decode_current_byte & 0x80) {
        result = 1;
    }
    mfx_decode_current_byte <<= 1;
    mfx_decode_bits_remaining--;
    return result;
}

/*--------------------------------------------------------------------------
 * mfx_feedback_data - The bytes of a data feedback window (0xCFE4)
 *
 * The captured bits come in pairs: data bit, quality bit. Within the first ten pairs the
 * data bits must show the start pattern 1 1 (1...) 0 1 0; then data_count bytes follow
 * (into decode_buf) and a CRC byte (mfx CRC: start 0xFF, polynomial 0x107). A quality
 * score starts at 100 and halves on each quality bit 0; it must stay above 19 (at most
 * two bad bits).
 *
 * Returns 0 when nothing was captured, 1 for a frame with a valid CRC, -1 otherwise.
 *------------------------------------------------------------------------*/
s16 mfx_feedback_data(u8 *decode_buf, u16 data_count)
{
    u8 crc;
    u8 sync_count;
    u8 state;
    u8 iteration;
    u16 bit_val, prev_bit;
    u16 i, j;
    u8 data_byte;
    u8 quality;

    if (track_capture_samples == 0) {
        return 0;
    }
    mfx_decode_read_ptr = (u16)timer_capture_buffer + 5;
    mfx_decode_bits_remaining = 0;

    crc = 0xFF;
    data_byte = 0;
    quality = 100;
    state = 0;
    sync_count = 1;

    iteration = sync_count;
    do {                    /* ten pairs: the count is compared before its increment */
        prev_bit = mfx_feedback_read_bit();
        bit_val = mfx_feedback_read_bit();
        if (bit_val == 0) {
            quality >>= 1;
        }

        /* the start pattern (the original: a jump table at 0xF350)
         *
         * state | data bit 0      | data bit 1
         * ------+-----------------+-------------
         *   0   | 0               | 1
         *   1   | 0               | 2
         *   2   | 3               | 2
         *   3   | 0               | 4
         *   4   | 5: found        | 2
         */
        if (state > 4) {
            continue;
        }

        switch (state) {
        case 0:
            if (prev_bit != 0) {
                state = 1;
            }
            break;

        case 1:
            if (prev_bit != 0) {
                state = 2;
            } else {
                state = 0;
            }
            break;

        case 2:
            if (prev_bit == 0) {
                state = 3;
            }
            break;

        case 3:
            if (prev_bit != 0) {
                state = 4;
            } else {
                state = 0;
            }
            break;

        case 4:
            if (prev_bit == 0) {
                state = 5;
                goto sync_done;
            } else {
                state = 2;
            }
            break;
        }
    } while (iteration++ <= 9);

sync_done:
    if (state != 5) {
        return -1;
    }

    for (i = 0; i < data_count; i++) {
        u8 *target = &decode_buf[i];

        for (j = 8; j > 0; j--) {
            u16 bit = mfx_feedback_read_bit();

            data_byte <<= 1;
            data_byte += (u8)bit;

            if (crc & 0x80) {
                crc <<= 1;
                crc ^= (u8)bit;
                crc ^= 0x07;
            } else {
                crc <<= 1;
                crc ^= (u8)bit;
            }

            bit_val = mfx_feedback_read_bit();
            if (bit_val == 0) {
                quality >>= 1;
            }
        }

        *target = data_byte;
    }

    /* the CRC byte */
    for (j = 8; j > 0; j--) {
        u16 bit = mfx_feedback_read_bit();
        data_byte = (data_byte << 1) | (u8)bit;

        bit_val = mfx_feedback_read_bit();
        if (bit_val == 0) {
            quality >>= 1;
        }
    }

    if (quality <= 0x13) {
        return -1;
    }
    if (crc != data_byte) {
        return -1;
    }
    return 1;
}
