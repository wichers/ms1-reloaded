/*==========================================================================
 * sys/vectors.c - the interrupt trampolines, 0xF740-0xF7FF (the .tramp section, gcc/ms1.ld)
 *
 * The bootloader's vector table (0xFF80 + 2n) points vector n at 0xF740 + 3n, so the
 * application has a 3-byte JMP per vector here, in vector order; the last one, the reset
 * vector's, is the application's entry (docs/bootloader.md). Vectors without a handler go to
 * unexpected_interrupt.
 *========================================================================*/

#include "../include/hcs12.h"

__asm__ (
"	.sect	.tramp,\"ax\"\n"
"	.rept	24\n"                           /* 0-23: 0xFF80-0xFFAE */
"	jmp	unexpected_interrupt\n"
"	.endr\n"
"	jmp	can0_tx_isr\n"                  /* 24: 0xFFB0 CAN0 transmit */
"	jmp	can0_rx_isr\n"                  /* 25: 0xFFB2 CAN0 receive */
"	jmp	can0_error_isr\n"               /* 26: 0xFFB4 CAN0 errors */
"	.rept	10\n"                           /* 27-36: 0xFFB6-0xFFC8 */
"	jmp	unexpected_interrupt\n"
"	.endr\n"
"	jmp	isr_ms_tick\n"                  /* 37: 0xFFCA modulus down counter */
"	.rept	6\n"                            /* 38-43: 0xFFCC-0xFFD6 */
"	jmp	unexpected_interrupt\n"
"	.endr\n"
"	jmp	isr_spi0_lcd\n"                 /* 44: 0xFFD8 SPI0 */
"	.rept	6\n"                            /* 45-50: 0xFFDA-0xFFE4 */
"	jmp	unexpected_interrupt\n"
"	.endr\n"
"	jmp	isr_timer_ch4_signal_gen\n"     /* 51: 0xFFE6 timer channel 4 */
"	.rept	3\n"                            /* 52-54: 0xFFE8-0xFFEC */
"	jmp	unexpected_interrupt\n"
"	.endr\n"
"	jmp	isr_feedback_capture\n"         /* 55: 0xFFEE timer channel 0 */
"	.rept	7\n"                            /* 56-62: 0xFFF0-0xFFFC */
"	jmp	unexpected_interrupt\n"
"	.endr\n"
"	jmp	_start\n"                       /* 63: 0xFFFE reset */

/* An interrupt with no handler: stop in background mode (the emulator halts and reports it),
 * or spin if BDM is off */
"	.sect	.text.unexpected_interrupt\n"
"	.globl	unexpected_interrupt\n"
"unexpected_interrupt:\n"
"	bgnd\n"
".Lvec_spin:\n"
"	bra	.Lvec_spin\n"
);
