/*==========================================================================
 * signal/feedback_capture.c - capturing an mfx feedback window (timer channel 0)
 *
 * The track signal interrupt (signal/signal_gen.c) starts and stops a capture at the staging
 * entries the generator marks for it (track_capture_cmd). Timer channel 0 captures the
 * receiver's clock (PT0); at each captured edge the capture interrupt stores
 *   bit mode (1):  the data (PT1) and quality (PB5) levels, two bits per edge, eight edges
 *                  to a word, shifted in from the right
 *   ATD mode (2):  the carrier level, ATD0 channel 2, a byte per edge
 * into timer_capture_buffer, until the stop or the end of the buffer; mfx_feedback.c
 * evaluates it. These routines are assembly: they run in the interrupts, and the moments
 * at which a capture starts and samples its inputs count in cycles.
 *
 * Functions:
 *   isr_feedback_capture          (0xDF13) - interrupt: one captured edge
 *   timer_capture_start_bit_mode  (0xDF67) - start a capture in bit mode
 *   timer_capture_start_adc_mode  (0xDF8F) - start a capture in ATD mode
 *   timer_capture_stop_and_align  (0xDF9A) - stop a bit mode capture: its samples, the last
 *                                            word's moved up to its top bits
 *   timer_capture_stop_and_count  (0xDFCA) - stop an ATD mode capture: its samples
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/asm.h"
#include "../include/globals.h"

#define CAPTURE_END "timer_capture_buffer+" ASM_NUM(TIMER_CAPTURE_SIZE)

__asm__ (
"	.sect	.text.isr_feedback_capture\n"
"	.globl	isr_feedback_capture\n"
"isr_feedback_capture:\n"
"	ldd	TC0\n"                          /* clears the flag (TFFCA) */
"	ldaa	timer_capture_mode\n"
"	cmpa	#1\n"
"	beq	.Lcap_bit\n"
"	cmpa	#2\n"
"	beq	.Lcap_atd\n"
".Lcap_done:\n"
"	rti\n"
".Lcap_bit:\n"
"	ldx	timer_capture_ptr\n"
"	ldd	0,x\n"
"	asld\n"
"	brclr	PTT,#2,.Lcap_data\n"
"	incb\n"
".Lcap_data:\n"
"	asld\n"
"	brclr	PORTB,#0x20,.Lcap_quality\n"
"	incb\n"
".Lcap_quality:\n"
"	std	0,x\n"
"	dec	timer_capture_count\n"
"	bne	.Lcap_done\n"
"	movb	#8,timer_capture_count\n"       /* the word is full: the next */
"	leax	2,x\n"
"	stx	timer_capture_ptr\n"
"	cpx	#" CAPTURE_END "\n"
"	bne	.Lcap_done\n"
".Lcap_off:\n"
"	bclr	TIE,#1\n"
"	bclr	TCTL4,#3\n"
"	bra	.Lcap_done\n"
".Lcap_atd:\n"
"	ldx	timer_capture_ptr\n"
"	movb	ATD0DR2H,1,x+\n"
"	stx	timer_capture_ptr\n"
"	cpx	#" CAPTURE_END "\n"
"	beq	.Lcap_off\n"
"	bra	.Lcap_done\n"

/* bit mode captures rising edges, ATD mode both */
"	.sect	.text.timer_capture_start_bit_mode\n"
"	.globl	timer_capture_start_bit_mode\n"
"	.globl	timer_capture_start_adc_mode\n"
"timer_capture_start_bit_mode:\n"
"	movb	#1,timer_capture_mode\n"
"	bset	TCTL4,#1\n"
".Lcap_start:\n"
"	ldx	#timer_capture_buffer\n"
"	ldab	#" ASM_NUM(TIMER_CAPTURE_SIZE) "/2\n"
".Lcap_clear:\n"
"	clr	1,x+\n"
"	clr	1,x+\n"
"	dbne	b,.Lcap_clear\n"
"	ldd	TC0\n"                          /* no edge from before */
"	movw	#timer_capture_buffer,timer_capture_ptr\n"
"	movb	#8,timer_capture_count\n"
"	bset	TIE,#1\n"
"	rts\n"
"timer_capture_start_adc_mode:\n"
"	movb	#2,timer_capture_mode\n"
"	bset	TCTL4,#3\n"
"	bra	.Lcap_start\n"

/* the samples: 8 per full word, and 8 - timer_capture_count in the last */
"	.sect	.text.timer_capture_stop_and_align\n"
"	.globl	timer_capture_stop_and_align\n"
"timer_capture_stop_and_align:\n"
"	bclr	TIE,#1\n"
"	bclr	TCTL4,#3\n"
"	clr	timer_capture_mode\n"
"	ldd	timer_capture_ptr\n"
"	subd	#timer_capture_buffer-2\n"
"	asld\n"
"	asld\n"
"	subb	timer_capture_count\n"
"	sbca	#0\n"
"	pshd\n"
"	ldaa	timer_capture_count\n"
"	cmpa	#8\n"
"	beq	.Lcap_aligned\n"
"	ldx	timer_capture_ptr\n"
"	ldd	0,x\n"
".Lcap_align:\n"
"	asld\n"
"	asld\n"
"	dec	timer_capture_count\n"
"	bne	.Lcap_align\n"
"	std	0,x\n"
".Lcap_aligned:\n"
"	puld\n"
"	rts\n"

"	.sect	.text.timer_capture_stop_and_count\n"
"	.globl	timer_capture_stop_and_count\n"
"timer_capture_stop_and_count:\n"
"	bclr	TIE,#1\n"
"	bclr	TCTL4,#3\n"
"	clr	timer_capture_mode\n"
"	ldd	timer_capture_ptr\n"
"	subd	#timer_capture_buffer\n"
"	rts\n"
);
