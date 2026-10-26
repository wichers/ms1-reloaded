/*==========================================================================
 * sys/crt0.c - start-up, restart and the way into the bootloader, at the start of the
 * application (the .crt0 section at 0x4000, gcc/ms1.ld)
 *
 * The bootloader jumps to _start after its checks (docs/bootloader.md). The layout follows
 * the original application's 0x4000-0x40A2, which is the m68hc1x GCC start-up:
 *   _start            the MMC registers as the original writes them (INITRM, INITRG and
 *                     MISC are write-once in normal mode, so the bootloader's values stay),
 *                     page $3D in the window for good, then the PLL for a 24 MHz bus,
 *                     engaged after a fixed wait
 *   crt_restart       (0x403C, also the soft-reset entry) the track signal and booster off,
 *                     .data copied, .bss cleared, the stack, main(1, {"main"})
 *   0x4077            a restart through the reset vector (nothing calls it)
 *   enter_bootloader  (0x407C) interrupts and the track signal off, RAM cleared, the stack
 *                     reset, then the bootloader's update entry from its flash metadata
 *                     (0xFF70), or 0xF8F6 if that is 0
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/asm.h"

/* the booster off before a restart (hcs12.h: TRACK_OUTPUT_OFF) */
#if MS1_RELEASE == 0x4010
#define BOOSTER_OFF "	bclr	PORTB,#0x80\n"        /* PB7 low */
#else
#define BOOSTER_OFF "	bclr	TIOS,#0x08\n"         /* channel 3 to input capture */
#endif

__asm__ (
"	.sect	.crt0,\"ax\"\n"
"	.globl	_start\n"
"_start:\n"
"	ldd	#0\n"
"	anda	#0x78\n"
"	staa	INITRG\n"
"	ldd	#0x1000\n"
"	anda	#0xF8\n"
"	staa	INITRM\n"
"	ldd	#0x0800\n"
"	anda	#0xF8\n"
"	oraa	#0x01\n"
"	staa	INITEE\n"                       /* EEPROM at 0x0800, on */
"	movb	#0x0D,MISC\n"
"	movb	#0x3D,PPAGE\n"
"	clr	CLKSEL\n"                       /* bus from the oscillator */
"	movb	#0x17,SYNR\n"
"	movb	#0x03,REFDV\n"                  /* PLL 48 MHz, bus 24 MHz */
"	ldd	#0xFFFF\n"
".Lcrt_lock:\n"
"	dbne	d,.Lcrt_lock\n"                 /* the PLL's lock time */
"	movb	#0xC0,CLKSEL\n"                 /* PLLSEL, PSTP */

"	.globl	crt_restart\n"
"crt_restart:\n"
"	orcc	#0x50\n"
"	bclr	PORTE,#0x04\n"
"	bclr	TIE,#0x10\n"                    /* timer channel 4 */
BOOSTER_OFF
"	ldx	#__data_image\n"
"	ldy	#__data_section_start\n"
".Lcrt_data:\n"
"	cpy	#_edata\n"
"	beq	.Lcrt_bss\n"
"	movb	1,x+,1,y+\n"
"	bra	.Lcrt_data\n"
".Lcrt_bss:\n"
"	ldy	#__bss_start\n"
".Lcrt_clear:\n"
"	cpy	#__bss_end\n"
"	beq	.Lcrt_main\n"
"	clr	1,y+\n"
"	bra	.Lcrt_clear\n"
".Lcrt_main:\n"
"	lds	#_stack\n"
"	movw	#crt_argv,2,-sp\n"
"	ldd	#1\n"
"	jsr	main\n"
".Lcrt_stop:\n"
"	bra	.Lcrt_stop\n"

"	ldy	0xFFFE\n"                       /* the reset vector */
"	jmp	0,y\n"

"	.globl	enter_bootloader\n"
"enter_bootloader:\n"
"	orcc	#0x50\n"
"	bclr	TIE,#0x10\n"
BOOSTER_OFF
"	bclr	PORTE,#0x04\n"
"	ldy	#0x1000\n"
".Lcrt_ram:\n"
"	cpy	#0x2000\n"
"	beq	.Lcrt_boot\n"
"	clr	1,y+\n"
"	bra	.Lcrt_ram\n"
".Lcrt_boot:\n"
"	lds	#0x2000\n"
"	ldy	0xFF70\n"
"	bne	.Lcrt_update\n"
"	ldy	#0xF8F6\n"
".Lcrt_update:\n"
"	jmp	0,y\n"

"	.sect	.rodata\n"
"crt_argv:\n"
"	.word	crt_argv0\n"
"crt_argv0:\n"
"	.asciz	\"main\"\n"
);
