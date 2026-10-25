; Start-up code for the GNU build, laid out as the original application's (0x4000-0x4076,
; itself the m68hc1x GCC start-up): the bootloader jumps to _start after its CRC check.
; MMC registers are written as the original does (INITRM/INITRG/MISC are write-once in
; normal mode, so the bootloader's values stay), the PLL is engaged after a fixed delay,
; then crt_restart (the original's 0x403C, also the soft-reset entry) copies .data,
; clears .bss, sets the stack and calls main(1, {"main"}). The original's 0x4077-0x40A2
; follows.

	.sect .crt0,"ax"
	.globl	_start
_start:
	ldd	#0
	anda	#0x78
	staa	0x0011		; INITRG
	ldd	#0x1000
	anda	#0xF8
	staa	0x0010		; INITRM
	ldd	#0x0800
	anda	#0xF8
	oraa	#0x01
	staa	0x0012		; INITEE: EEPROM at 0x0800, EEON
	movb	#0x0D,0x0013	; MISC
	movb	#0x3D,0x0030	; PPAGE: page $3D in the window, for good
	clr	0x0039		; CLKSEL: bus from the oscillator
	movb	#0x17,0x0034	; SYNR
	movb	#0x03,0x0035	; REFDV: PLL 48 MHz, bus 24 MHz
	ldd	#0xFFFF
1:	dbne	d,1b		; PLL lock time
	movb	#0xC0,0x0039	; CLKSEL: PLLSEL, PSTP

	.globl	crt_restart
crt_restart:
	orcc	#0x50
	bclr	0x0008,#0x04	; PORTE bit 2
	bclr	0x004C,#0x10	; TIE: timer channel 4
	bclr	0x0040,#0x08	; TIOS: channel 3 to input capture
	ldx	#__data_image
	ldy	#__data_section_start
2:	cpy	#_edata
	beq	3f
	movb	1,x+,1,y+
	bra	2b
3:	ldy	#__bss_start
4:	cpy	#__bss_end
	beq	5f
	clr	1,y+
	bra	4b
5:	lds	#_stack
	movw	#crt_argv,2,-sp
	ldd	#1
	jsr	main
6:	bra	6b

; 0x4077: restart through the reset vector (nothing calls it)
	ldy	0xFFFE
	jmp	0,y

; 0x407C: leave the application for the bootloader (firmware update): interrupts and the
; track signal off, RAM cleared, stack reset, then the bootloader's entry from its flash
; metadata (0xFF70), or 0xF8F6 if that is 0.
	.globl	enter_bootloader
enter_bootloader:
	orcc	#0x50
	bclr	0x004C,#0x10	; TIE: timer channel 4
	bclr	0x0040,#0x08	; TIOS: channel 3 to input capture
	bclr	0x0008,#0x04	; PORTE bit 2
	ldy	#0x1000
7:	cpy	#0x2000
	beq	8f
	clr	1,y+
	bra	7b
8:	lds	#0x2000
	ldy	0xFF70
	bne	9f
	ldy	#0xF8F6
9:	jmp	0,y

	.sect .rodata
crt_argv:
	.word	crt_argv0
crt_argv0:
	.asciz	"main"
