; Interrupt trampolines, 0xF740-0xF7FF: the bootloader's vector table (0xFF80 + 2n) points
; every vector at 0xF740 + 3n, so the application provides one JMP per vector here. Unused
; vectors go to unexpected_interrupt.

	.sect .tramp,"ax"
	jmp	unexpected_interrupt            ;  0 0xFF80
	jmp	unexpected_interrupt            ;  1 0xFF82
	jmp	unexpected_interrupt            ;  2 0xFF84
	jmp	unexpected_interrupt            ;  3 0xFF86
	jmp	unexpected_interrupt            ;  4 0xFF88
	jmp	unexpected_interrupt            ;  5 0xFF8A
	jmp	unexpected_interrupt            ;  6 0xFF8C
	jmp	unexpected_interrupt            ;  7 0xFF8E
	jmp	unexpected_interrupt            ;  8 0xFF90
	jmp	unexpected_interrupt            ;  9 0xFF92
	jmp	unexpected_interrupt            ; 10 0xFF94
	jmp	unexpected_interrupt            ; 11 0xFF96
	jmp	unexpected_interrupt            ; 12 0xFF98
	jmp	unexpected_interrupt            ; 13 0xFF9A
	jmp	unexpected_interrupt            ; 14 0xFF9C
	jmp	unexpected_interrupt            ; 15 0xFF9E
	jmp	unexpected_interrupt            ; 16 0xFFA0
	jmp	unexpected_interrupt            ; 17 0xFFA2
	jmp	unexpected_interrupt            ; 18 0xFFA4
	jmp	unexpected_interrupt            ; 19 0xFFA6
	jmp	unexpected_interrupt            ; 20 0xFFA8
	jmp	unexpected_interrupt            ; 21 0xFFAA
	jmp	unexpected_interrupt            ; 22 0xFFAC
	jmp	unexpected_interrupt            ; 23 0xFFAE
	jmp	can0_tx_isr                     ; 24 0xFFB0 CAN0 transmit
	jmp	can0_rx_isr                     ; 25 0xFFB2 CAN0 receive
	jmp	can0_error_isr                  ; 26 0xFFB4 CAN0 errors
	jmp	unexpected_interrupt            ; 27 0xFFB6
	jmp	unexpected_interrupt            ; 28 0xFFB8
	jmp	unexpected_interrupt            ; 29 0xFFBA
	jmp	unexpected_interrupt            ; 30 0xFFBC
	jmp	unexpected_interrupt            ; 31 0xFFBE
	jmp	unexpected_interrupt            ; 32 0xFFC0
	jmp	unexpected_interrupt            ; 33 0xFFC2
	jmp	unexpected_interrupt            ; 34 0xFFC4
	jmp	unexpected_interrupt            ; 35 0xFFC6
	jmp	unexpected_interrupt            ; 36 0xFFC8
	jmp	isr_ms_tick         ; 37 0xFFCA modulus down-counter
	jmp	unexpected_interrupt            ; 38 0xFFCC
	jmp	unexpected_interrupt            ; 39 0xFFCE
	jmp	unexpected_interrupt            ; 40 0xFFD0
	jmp	unexpected_interrupt            ; 41 0xFFD2
	jmp	unexpected_interrupt            ; 42 0xFFD4
	jmp	unexpected_interrupt            ; 43 0xFFD6
	jmp	isr_spi0_lcd            ; 44 0xFFD8 SPI0
	jmp	unexpected_interrupt            ; 45 0xFFDA
	jmp	unexpected_interrupt            ; 46 0xFFDC
	jmp	unexpected_interrupt            ; 47 0xFFDE
	jmp	unexpected_interrupt            ; 48 0xFFE0
	jmp	unexpected_interrupt            ; 49 0xFFE2
	jmp	unexpected_interrupt            ; 50 0xFFE4
	jmp	isr_timer_ch4_signal_gen        ; 51 0xFFE6 timer channel 4
	jmp	unexpected_interrupt            ; 52 0xFFE8
	jmp	unexpected_interrupt            ; 53 0xFFEA
	jmp	unexpected_interrupt            ; 54 0xFFEC
	jmp	isr_feedback_capture     ; 55 0xFFEE timer channel 0
	jmp	unexpected_interrupt            ; 56 0xFFF0
	jmp	unexpected_interrupt            ; 57 0xFFF2
	jmp	unexpected_interrupt            ; 58 0xFFF4
	jmp	unexpected_interrupt            ; 59 0xFFF6
	jmp	unexpected_interrupt            ; 60 0xFFF8
	jmp	unexpected_interrupt            ; 61 0xFFFA
	jmp	unexpected_interrupt            ; 62 0xFFFC
	jmp	_start                          ; 63 - application entry (bootloader, after its CRC check)

; An interrupt with no handler: stop in background mode (the emulator halts and reports it),
; or spin if BDM is not enabled.
	.sect .text
	.globl	unexpected_interrupt
unexpected_interrupt:
	bgnd
1:	bra	1b
