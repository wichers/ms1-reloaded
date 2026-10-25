/*==========================================================================
 * hal/serial_debug.c - SCI0 Serial Debug Output
 *
 * Simple polled UART transmit on PS1/TXD0 for debug printf.
 * Bus clock: 24 MHz. Default baud: 115200.
 *
 * Usage:
 *   serial_init();
 *   serial_puts("Hello MS1!\r\n");
 *   serial_puthex16(0xDEAD);
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/serial_debug.h"


#ifdef MS1_DEBUG_SERIAL   /* SCI0 115200 baud; otherwise include/serial_debug.h removes every call */

/*--------------------------------------------------------------------------
 * serial_init - Initialize SCI0 for TX-only at specified baud rate
 *
 * Baud rate = bus_clock / (16 * SCI0BD)
 * 24 MHz / (16 * 13) = 115384 (~115200 baud, 0.16% error)
 * 24 MHz / (16 * 156) = 9615 (~9600 baud)
 *------------------------------------------------------------------------*/
void serial_init(void)
{
    /* MC9S12D64 with 4 MHz crystal, PLL: SYNR=0x17, REFDV=0x03
     * PLLCLK = 2 * 4MHz * 24 / 4 = 48 MHz
     * Bus clock = PLLCLK / 2 = 24 MHz
     * Baud = bus_clock / (16 * SCI0BD)
     * At 24 MHz: SCI0BD=156 -> 9615 baud (~9600)
     */

    /* Disable SCI first for clean config */
    SCI0CR2 = 0x00;
    SCI0CR1 = 0;         /* 8N1, no parity, no loops */

    /* Auto-detect bus clock by checking PLL lock status.
     * CRGFLG bit 3 (LOCK) = 1 if PLL is locked.
     * If locked: bus = 24 MHz (4 MHz * 2 * 24 / 4 / 2)
     * If not:    bus = 2 MHz  (4 MHz oscillator / 2) */
    SCI0BDH = 0;
    if (CRGFLG & 0x08) {
        /* PLL locked: bus = 24 MHz */
        SCI0BDL = 13;    /* 24M / 16 / 13 = 115384 baud (~115200) */
    } else {
        /* PLL not locked: bus = 2 MHz */
        SCI0BDL = 1;     /* 2M / 16 / 1 = 125000 baud (close enough) */
    }

    SCI0CR2 = 0x08;      /* TE=1 (transmitter enable), no interrupts */
}

/*--------------------------------------------------------------------------
 * serial_putc - Send one byte (polled, blocks until TX buffer empty)
 *------------------------------------------------------------------------*/
void serial_putc(u8 ch)
{
    while (!(SCI0SR1 & 0x80)) {
        /* Wait for TDRE (Transmit Data Register Empty) */
    }
    SCI0DRL = ch;
    /* Wait for transmit complete (TC flag) to ensure byte is fully sent */
    while (!(SCI0SR1 & 0x40)) {
    }
}

/*--------------------------------------------------------------------------
 * serial_puts - Send a null-terminated string
 *------------------------------------------------------------------------*/
void serial_puts(const u8 *str)
{
    while (*str) {
        serial_putc(*str++);
    }
}

/*--------------------------------------------------------------------------
 * serial_puthex4 - Send one hex nibble (0-F)
 *------------------------------------------------------------------------*/
void serial_puthex4(u8 nibble)
{
    nibble &= 0x0F;
    serial_putc(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
}

/*--------------------------------------------------------------------------
 * serial_puthex8 - Send a byte as 2 hex digits
 *------------------------------------------------------------------------*/
void serial_puthex8(u8 val)
{
    serial_puthex4(val >> 4);
    serial_puthex4(val);
}

/*--------------------------------------------------------------------------
 * serial_puthex16 - Send a 16-bit word as 4 hex digits
 *------------------------------------------------------------------------*/
void serial_puthex16(u16 val)
{
    serial_puthex8((u8)(val >> 8));
    serial_puthex8((u8)val);
}

/*--------------------------------------------------------------------------
 * serial_putdec - Send a 16-bit unsigned value as decimal
 *------------------------------------------------------------------------*/
void serial_putdec(u16 val)
{
    u8 buf[6];
    u8 i = 0;

    if (val == 0) {
        serial_putc('0');
        return;
    }
    while (val > 0) {
        buf[i++] = (u8)(val % 10) + '0';
        val /= 10;
    }
    while (i > 0) {
        serial_putc(buf[--i]);
    }
}

#endif
