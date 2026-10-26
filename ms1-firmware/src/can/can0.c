/*==========================================================================
 * can/can0.c - the CAN0 controller (MSCAN) and its frame rings
 *
 * Frames go through two rings of MSCAN frame images (globals.h: can_tx_ring, can_rx_ring),
 * each with a read and a write pointer; a ring is empty when they are equal. The receive
 * interrupt copies every received frame into can_rx_ring; the transmit interrupt, enabled
 * while can_tx_ring holds frames, sends them through transmit buffer 0. The interrupt
 * handlers are assembly: they delay the track signal interrupt (README: Interrupt timing).
 *
 * Functions:
 *   can0_error_isr            (0xDD10) - interrupt: receive overrun
 *   can0_tx_isr               (0xDD1B) - interrupt: the next frame of can_tx_ring
 *   can0_rx_isr               (0xDD61) - interrupt: the received frame into can_rx_ring
 *   can_poll                  (0xDD98) - both handlers, called (nothing calls it)
 *   can_controller_full_init  (0xDDB5) - the controller, the rings, the filters
 *   can_tx_enqueue_frame      (0xDE43) - a frame into can_tx_ring
 *   can_rx_dequeue_frame      (0xDEAC) - the next frame of can_rx_ring
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/asm.h"
#include "../include/globals.h"
#include "../include/mcan.h"

#define REG16(r)        (*(volatile u16 *)&(r))

#define CTL0_INITRQ     0x01
#define CTL0_SLPRQ      0x02
#define CTL0_RXFRM      0x80
#define CTL1_INITAK     0x01
#define CTL1_SLPAK      0x02
#define CTL1_CLKSRC     0x40        /* the bus clock */
#define CTL1_CANE       0x80
#define BTR0_SJW4       0xC0
#define BTR1_24TQ       0xFE        /* 3 samples, 15 + 8 time quanta after the sync quantum */
#define RIER_RXFIE      0x01
#define RIER_OVRIE      0x02
#define TIER_TXEIE0     0x01
#define IDR1_IDE        0x08        /* an extended (29-bit) identifier */
#define IDR1_SRR_IDE    0x18

__asm__ (
"	.sect	.text.can0_error_isr\n"
"	.globl	can0_error_isr\n"
"can0_error_isr:\n"
"	movb	#2,CAN0RFLG\n"                  /* OVRIF: the only error interrupt enabled */
"	rti\n"

"	.sect	.text.can0_tx_isr\n"
"	.globl	can0_tx_isr\n"
"can0_tx_isr:\n"
"	brclr	CAN0TFLG,#1,.Ltx_done\n"        /* transmit buffer 0 still busy */
"	ldx	can_tx_ring_read\n"
"	movb	#1,CAN0TBSEL\n"                 /* buffer 0 at CAN0TXIDR0 */
"	ldy	#CAN0TXIDR0\n"
"	movw	2,x+,2,y+\n"                    /* IDR0-3, DSR0-7, DLR */
"	movw	2,x+,2,y+\n"
"	movw	2,x+,2,y+\n"
"	movw	2,x+,2,y+\n"
"	movw	2,x+,2,y+\n"
"	movw	2,x+,2,y+\n"
"	movb	1,x+,1,y+\n"
"	movb	#1,CAN0TFLG\n"                  /* send it */
"	cpx	#can_tx_ring+" ASM_NUM(CAN_TX_RING_SIZE) "\n"
"	bne	.Ltx_next\n"
"	ldx	#can_tx_ring\n"
".Ltx_next:\n"
"	stx	can_tx_ring_read\n"
"	cpx	can_tx_ring_write\n"
"	bne	.Ltx_done\n"
"	bclr	CAN0TIER,#1\n"                  /* the ring is empty */
".Ltx_done:\n"
"	rti\n"

"	.sect	.text.can0_rx_isr\n"
"	.globl	can0_rx_isr\n"
"can0_rx_isr:\n"
"	ldy	can_rx_ring_write\n"
"	movw	CAN0RXIDR0,2,y+\n"
"	movw	CAN0RXIDR2,2,y+\n"
"	movw	CAN0RXDSR0,2,y+\n"
"	movw	CAN0RXDSR2,2,y+\n"
"	movw	CAN0RXDSR4,2,y+\n"
"	movw	CAN0RXDSR6,2,y+\n"
"	movb	CAN0RXDLR,1,y+\n"
"	movb	#1,CAN0RFLG\n"                  /* RXF: give the buffer back */
"	cpy	#can_rx_ring+" ASM_NUM(CAN_RX_RING_SIZE) "\n"
"	bne	.Lrx_next\n"
"	ldy	#can_rx_ring\n"
".Lrx_next:\n"
"	sty	can_rx_ring_write\n"
"	rti\n"

/* Each handler entered as if its interrupt came: an RTI frame of the registers, the
 * receive handler's returning into the transmit handler's, that one's back here */
"	.sect	.text.can_poll\n"
"	.globl	can_poll\n"
"can_poll:\n"
"	movw	#.Lpoll_tx,2,-sp\n"
"	pshy\n"
"	pshx\n"
"	pshd\n"
"	pshc\n"
"	orcc	#0x50\n"
"	jmp	can0_rx_isr\n"
".Lpoll_tx:\n"
"	movw	#.Lpoll_done,2,-sp\n"
"	pshy\n"
"	pshx\n"
"	pshd\n"
"	pshc\n"
"	orcc	#0x50\n"
"	jmp	can0_tx_isr\n"
".Lpoll_done:\n"
"	rts\n"
);

/* bitrate_100: the bit rate in 100 bit/s; 24 time quanta a bit at the 24 MHz bus */
void can_controller_full_init(u16 bitrate_100)
{
    u16 ccr;

    IRQ_SAVE(ccr);
    can_tx_ring_read = (u16)can_tx_ring;
    can_tx_ring_write = (u16)can_tx_ring;
    can_rx_ring_read = (u16)can_rx_ring;
    can_rx_ring_write = (u16)can_rx_ring;
    CAN0CTL1 |= CTL1_CANE;
    CAN0TARQ = 7;                       /* abort what the transmit buffers hold */
    CAN0CTL0 |= CTL0_SLPRQ;
    while (!(CAN0CTL1 & CTL1_SLPAK))
        ;
    CAN0CTL1 = CTL1_CANE | CTL1_CLKSRC;
    CAN0CTL0 = CTL0_RXFRM | CTL0_INITRQ;
    while (!(CAN0CTL1 & CTL1_INITAK))
        ;
    CAN0BTR0 = BTR0_SJW4 | (u8)(10000 / bitrate_100 - 1);
    CAN0BTR1 = BTR1_24TQ;
    /* two 32-bit filters on SRR and IDE alone: every extended frame, every standard data
     * frame */
    REG16(CAN0IDMR0) = 0xFFE7;
    REG16(CAN0IDMR2) = 0xFFFF;
    REG16(CAN0IDMR4) = 0xFFE7;
    REG16(CAN0IDMR6) = 0xFFFF;
    REG16(CAN0IDAR0) = IDR1_SRR_IDE;
    REG16(CAN0IDAR2) = 0;
    REG16(CAN0IDAR4) = 0;
    REG16(CAN0IDAR6) = 0;
    CAN0CTL0 = 0;
    while (CAN0CTL1 & CTL1_INITAK)
        ;
    CAN0RIER = RIER_RXFIE | RIER_OVRIE;
    IRQ_RESTORE(ccr);
}

/* id a 29-bit identifier, or a standard one as -id. Returns 0 when the ring is full. */
u16 can_tx_enqueue_frame(u32 id, const u8 *data, u16 dlc)
{
    u8 *f = (u8 *)can_tx_ring_write;
    u16 hi = id >> 16, lo = id;
    u16 ccr;
    u8 i;

    if (!(hi & 0x8000)) {
        f[0] = hi >> 5;                                     /* ID28-21 */
        f[1] = (hi << 3 & 0xE0) | IDR1_SRR_IDE | (hi << 1 & 6) | lo >> 15;  /* ID20-18, ID17-15 */
        f[2] = lo >> 7;                                     /* ID14-7 */
        f[3] = lo << 1;                                     /* ID6-0, RTR 0 */
    } else {
        lo = -lo;
        f[0] = lo >> 3;                                     /* ID10-3 */
        f[1] = lo << 5;                                     /* ID2-0, RTR 0, IDE 0 */
    }
    for (i = 0; i < 8; i++)
        f[4 + i] = data[i];
    f[12] = dlc;
    f += CAN_FRAME_SIZE;
    if (f == can_tx_ring + CAN_TX_RING_SIZE)
        f = can_tx_ring;
    if ((u16)f == can_tx_ring_read)
        return 0;
    IRQ_SAVE(ccr);
    can_tx_ring_write = (u16)f;
    CAN0TIER |= TIER_TXEIE0;
    IRQ_RESTORE(ccr);
    return 1;
}

/* The next received frame to dest: the identifier as can_tx_enqueue_frame takes it (a
 * standard one as -(IDR0:IDR1)), the 8 data bytes, the length. Returns 0 when the ring is
 * empty; with dest NULL only whether it is, keeping the frame. */
u16 can_rx_dequeue_frame(u8 *dest)
{
    const u8 *f = (const u8 *)can_rx_ring_read;
    u16 hi, lo;
    u8 i;

    if ((u16)f == can_rx_ring_write)
        return 0;
    if (dest == NULL)
        return 1;
    if (f[1] & IDR1_IDE) {
        hi = f[0] << 5 | (f[1] >> 3 & 0x1C) | (f[1] >> 1 & 3);
        lo = (u16)f[1] << 15 | f[2] << 7 | f[3] >> 1;
    } else {
        hi = 0xFFFF;
        lo = -(f[0] << 8 | f[1]);
    }
    *(u16 *)&dest[0] = hi;
    *(u16 *)&dest[2] = lo;
    for (i = 0; i < 8; i++)
        dest[4 + i] = f[4 + i];
    dest[12] = f[12] & 15;
    f += CAN_FRAME_SIZE;
    if (f == can_rx_ring + CAN_RX_RING_SIZE)
        f = can_rx_ring;
    can_rx_ring_read = (u16)f;
    return 1;
}
