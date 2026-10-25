/* MSCAN0 (S12MSCANV2) on an error-free bus: initialization and sleep handshakes, bit timing from
 * CANBTR0/1, three transmit buffers (CANTBSEL, priority by TBPR then buffer number), the
 * five-deep receive FIFO with acceptance filtering (CANIDAC: 2x32, 4x16, 8x8 bit, closed),
 * receive overrun, listen-only and loopback. A transmitted frame takes its unstuffed bit count
 * on the bus and then goes to the board (bus model); received frames arrive whole from the
 * board. Error counters stay 0; time stamps (TIME) and wake-up are not modelled. */
#include <string.h>

#include "mcu.h"

enum {
	CTL0 = 0x140, CTL1, BTR0, BTR1, RFLG, RIER, TFLG, TIER, TARQ, TAAK, TBSEL, IDAC,
	MISC = 0x14D, RXERR = 0x14E, TXERR = 0x14F, IDAR0 = 0x150, IDMR0 = 0x154, IDAR4 = 0x158, IDMR4 = 0x15C,
	RXFG = 0x160, TXFG = 0x170,
};
enum { INITRQ = 0x01, SLPRQ = 0x02, SYNCH = 0x10, RXFRM = 0x80 };   /* CTL0 */
enum { INITAK = 0x01, SLPAK = 0x02, LISTEN = 0x10, LOOPB = 0x20, CLKSRC = 0x40, CANE = 0x80 };   /* CTL1 */
enum { RXF = 0x01, OVRIF = 0x02, CSCIF = 0x40, WUPIF = 0x80 };   /* RFLG */


static uint64_t bit_ticks(mcu_t *m)
{
	uint8_t b0 = m->regs[BTR0], b1 = m->regs[BTR1];
	uint64_t tq = (uint64_t)((b0 & 0x3F) + 1) *
		((m->regs[CTL1] & CLKSRC) ? m->bus_ticks : TICKS_PER_OSC);
	return tq * (1 + ((b1 & 0x0F) + 1) + ((b1 >> 4 & 7) + 1));
}

static bool init_mode(mcu_t *m) { return m->regs[CTL1] & INITAK; }

static void update_irqs(mcu_t *m)
{
	uint8_t rf = m->regs[RFLG], ri = m->regs[RIER];
	mcu_irq(m, 0xFFB0, m->regs[TFLG] & m->regs[TIER] & 7);
	mcu_irq(m, 0xFFB2, (rf & RXF) && (ri & RXF));
	mcu_irq(m, 0xFFB4, ((rf & CSCIF) && (ri & CSCIF)) || ((rf & OVRIF) && (ri & OVRIF)));
	mcu_irq(m, 0xFFB6, (rf & WUPIF) && (ri & WUPIF));
}

/* foreground receive buffer <- oldest FIFO entry */
static void show_rx(mcu_t *m)
{
	mscan_t *c = &m->can0;
	if (!c->rx_count)
		return;
	can_frame_t *f = &c->rx[c->rx_head];
	memcpy(&m->regs[RXFG], f->idr, 4);
	memcpy(&m->regs[RXFG + 4], f->dsr, 8);
	m->regs[RXFG + 12] = f->dlr;
	m->regs[RFLG] |= RXF;
}

static int selected_tx(mcu_t *m)
{
	uint8_t s = m->regs[TBSEL] & 7;
	return s ? __builtin_ctz(s) : -1;
}

static void tx_fg_save(mcu_t *m)
{
	int b = selected_tx(m);
	if (b < 0)
		return;
	can_frame_t *f = &m->can0.tx[b];
	memcpy(f->idr, &m->regs[TXFG], 4);
	memcpy(f->dsr, &m->regs[TXFG + 4], 8);
	f->dlr = m->regs[TXFG + 12];
	f->tbpr = m->regs[TXFG + 13];
}

static void tx_fg_load(mcu_t *m)
{
	int b = selected_tx(m);
	if (b < 0)
		return;
	can_frame_t *f = &m->can0.tx[b];
	memcpy(&m->regs[TXFG], f->idr, 4);
	memcpy(&m->regs[TXFG + 4], f->dsr, 8);
	m->regs[TXFG + 12] = f->dlr;
	m->regs[TXFG + 13] = f->tbpr;
}

static unsigned frame_bits(const can_frame_t *f)
{
	bool ext = f->idr[1] & 0x08;
	bool rtr = ext ? (f->idr[3] & 1) : (f->idr[1] & 0x10);
	unsigned n = rtr ? 0 : (f->dlr & 0x0F) > 8 ? 8 : (f->dlr & 0x0F);
	return (ext ? 67u : 47u) + 8 * n;   /* incl. EOF and intermission, without stuff bits */
}

/* Start the highest-priority pending buffer if the bus is free. */
static void tx_start(mcu_t *m)
{
	mscan_t *c = &m->can0;
	if (c->tx_busy >= 0 || init_mode(m) || !(m->regs[CTL0] & SYNCH) || (m->regs[CTL1] & LISTEN) ||
	    (m->regs[CTL0] & SLPRQ))
		return;
	uint8_t pending = (uint8_t)(~m->regs[TFLG] & 7);
	int best = -1;
	for (int b = 0; b < 3; b++)
		if ((pending >> b & 1) && (best < 0 || c->tx[b].tbpr < c->tx[best].tbpr))
			best = b;
	if (best < 0)
		return;
	c->tx_busy = best;
	mcu_schedule(m, EV_CAN0, m->now + frame_bits(&c->tx[best]) * bit_ticks(m));
}

static bool accept(mcu_t *m, const uint8_t idr[4], int *hit)
{
	const uint8_t *ar = &m->regs[IDAR0], *mr = &m->regs[IDMR0];
	bool ext = idr[1] & 0x08;
	int idam = m->regs[IDAC] >> 4 & 3;
	static const int bank[8] = { 0, 1, 2, 3, 8, 9, 10, 11 };   /* IDAR/IDMR byte offsets */
	switch (idam) {
	case 0:                                                    /* two 32-bit filters */
		for (int f = 0; f < 2; f++) {
			bool ok = true;
			for (int i = 0; i < (ext ? 4 : 2); i++)
				ok &= ((idr[i] ^ ar[bank[4 * f + i]]) & ~mr[bank[4 * f + i]]) == 0;
			if (ok) { *hit = f; return true; }
		}
		return false;
	case 1:                                                    /* four 16-bit filters */
		for (int f = 0; f < 4; f++) {
			int a = bank[2 * f], b = bank[2 * f + 1];
			if (((idr[0] ^ ar[a]) & ~mr[a]) == 0 && ((idr[1] ^ ar[b]) & ~mr[b]) == 0) { *hit = f; return true; }
		}
		return false;
	case 2:                                                    /* eight 8-bit filters */
		for (int f = 0; f < 8; f++)
			if (((idr[0] ^ ar[bank[f]]) & ~mr[bank[f]]) == 0) { *hit = f; return true; }
		return false;
	default:
		return false;
	}
}

static bool rx_store(mcu_t *m, const uint8_t idr[4], uint8_t dlc, const uint8_t *data)
{
	mscan_t *c = &m->can0;
	int hit;
	if (!accept(m, idr, &hit))
		return false;
	if (c->rx_count == CAN_RXFIFO) {
		m->regs[RFLG] |= OVRIF;
		update_irqs(m);
		return false;
	}
	can_frame_t *f = &c->rx[(c->rx_head + c->rx_count) % CAN_RXFIFO];
	memcpy(f->idr, idr, 4);
	memset(f->dsr, 0, 8);
	memcpy(f->dsr, data, (dlc & 0x0F) > 8 ? 8 : (dlc & 0x0F));
	f->dlr = dlc & 0x0F;
	c->rx_count++;
	m->regs[IDAC] = (uint8_t)((m->regs[IDAC] & 0x30) | hit);
	m->regs[CTL0] |= RXFRM;
	if (c->rx_count == 1)
		show_rx(m);
	update_irqs(m);
	return true;
}

bool mscan_receive(mcu_t *m, const uint8_t idr[4], uint8_t dlc, const uint8_t *data)
{
	if (!(m->regs[CTL1] & CANE) || init_mode(m) || !(m->regs[CTL0] & SYNCH) || (m->regs[CTL1] & LOOPB))
		return false;
	if (m->regs[CTL1] & SLPAK)
		return false;   /* wake-up not modelled */
	return rx_store(m, idr, dlc, data);
}

void mscan_event(mcu_t *m)
{
	mscan_t *c = &m->can0;
	if (m->can0.synch_due && m->now >= m->can0.synch_due) {
		m->can0.synch_due = 0;
		m->regs[CTL0] |= SYNCH;
	}
	if (c->tx_busy >= 0) {
		can_frame_t *f = &c->tx[c->tx_busy];
		m->regs[TFLG] |= (uint8_t)(1 << c->tx_busy);
		c->tx_busy = -1;
		if (m->regs[CTL1] & LOOPB)
			rx_store(m, f->idr, f->dlr, f->dsr);
		else if (m->board.can0_tx)
			m->board.can0_tx(m->board.ctx, f->idr, f->dlr & 0x0F, f->dsr);
	}
	tx_start(m);
	if (m->can0.synch_due && m->ev_when[EV_CAN0] > m->can0.synch_due)
		mcu_schedule(m, EV_CAN0, m->can0.synch_due);
	update_irqs(m);
}

static void enter_init(mcu_t *m)
{
	mscan_t *c = &m->can0;
	m->regs[CTL1] |= INITAK;
	m->regs[CTL0] = INITRQ;
	m->regs[RFLG] = 0;
	m->regs[RIER] = 0;
	m->regs[TFLG] = 7;
	m->regs[TIER] = 0;
	m->regs[TARQ] = 0;
	m->regs[TAAK] = 0;
	m->regs[TBSEL] = 0;
	c->tx_busy = -1;
	c->rx_count = c->rx_head = 0;
	m->can0.synch_due = 0;
	mcu_schedule(m, EV_CAN0, UINT64_MAX);
	update_irqs(m);
}

static uint8_t can_read(mcu_t *m, uint16_t off)
{
	if (off == TXERR || off == RXERR)
		return 0;
	return m->regs[off];
}

static void can_write(mcu_t *m, uint16_t off, uint8_t v)
{
	mscan_t *c = &m->can0;
	switch (off) {
	case CTL0: {
		uint8_t old = m->regs[CTL0];
		if ((old & INITRQ) && !(v & INITRQ) && !init_mode(m))
			v |= INITRQ;                                  /* cannot clear before INITAK */
		if ((old & SLPRQ) && !(v & SLPRQ) && !(m->regs[CTL1] & SLPAK))
			v |= SLPRQ;                                   /* cannot clear before SLPAK */
		m->regs[CTL0] = (uint8_t)((old & (SYNCH | 0x40)) | (v & 0x2F) | (old & RXFRM & ~v));
		if ((v & INITRQ) && !(old & INITRQ)) {
			enter_init(m);
			m->regs[CTL0] |= v & 0x2E;
		} else if (!(v & INITRQ) && (old & INITRQ)) {
			m->regs[CTL1] &= (uint8_t)~INITAK;            /* leave init: synchronize to the bus */
			m->regs[CTL0] &= (uint8_t)~SYNCH;
			m->can0.synch_due = m->now + 11 * bit_ticks(m);
			if (m->can0.synch_due < m->ev_when[EV_CAN0])
				mcu_schedule(m, EV_CAN0, m->can0.synch_due);
		}
		if ((v & SLPRQ) && c->tx_busy < 0)                 /* bus idle: sleep at once */
			m->regs[CTL1] |= SLPAK;
		if (!(m->regs[CTL0] & SLPRQ))
			m->regs[CTL1] &= (uint8_t)~SLPAK;
		tx_start(m);
		break;
	}
	case CTL1:
		if (!init_mode(m))
			break;
		v &= 0xF4;                                        /* INITAK, SLPAK read-only */
		if (m->regs[CTL1] & CANE)
			v |= CANE;                                    /* CANE: write once */
		m->regs[CTL1] = (uint8_t)(v | (m->regs[CTL1] & (INITAK | SLPAK)));
		break;
	case BTR0: case BTR1: case IDAC:
	case IDAR0: case IDAR0 + 1: case IDAR0 + 2: case IDAR0 + 3:
	case IDMR0: case IDMR0 + 1: case IDMR0 + 2: case IDMR0 + 3:
	case IDAR4: case IDAR4 + 1: case IDAR4 + 2: case IDAR4 + 3:
	case IDMR4: case IDMR4 + 1: case IDMR4 + 2: case IDMR4 + 3:
		if (init_mode(m))
			m->regs[off] = off == IDAC ? (v & 0x30) : v;
		break;
	case RFLG:                                            /* write 1 to clear */
		if (v & RXF && (m->regs[RFLG] & RXF)) {
			m->regs[RFLG] &= (uint8_t)~RXF;
			c->rx_head = (c->rx_head + 1) % CAN_RXFIFO;
			c->rx_count--;
			show_rx(m);
		}
		m->regs[RFLG] &= (uint8_t)~(v & (WUPIF | CSCIF | OVRIF));
		break;
	case RIER: case TIER:
		m->regs[off] = v;
		break;
	case TFLG:                                            /* write 1: buffer ready for transmission */
		tx_fg_save(m);
		m->regs[TFLG] &= (uint8_t)~(v & 7);
		tx_start(m);
		break;
	case TARQ:                                            /* abort: pending, not the one on the bus */
		for (int b = 0; b < 3; b++)
			if ((v >> b & 1) && !(m->regs[TFLG] >> b & 1) && b != c->tx_busy) {
				m->regs[TFLG] |= (uint8_t)(1 << b);
				m->regs[TAAK] |= (uint8_t)(1 << b);
			}
		break;
	case TBSEL: {
		tx_fg_save(m);
		uint8_t s = v & m->regs[TFLG] & 7;
		m->regs[TBSEL] = s ? (uint8_t)(s & -s) : 0;
		tx_fg_load(m);
		break;
	}
	case TAAK: case MISC: case RXERR: case TXERR:
		break;
	default:
		if (off >= TXFG && off < TXFG + 14)
			m->regs[off] = v;                             /* foreground transmit buffer */
		/* receive buffer and time stamps: read-only */
	}
	update_irqs(m);
}

void mscan_init(mcu_t *m)
{
	mcu_hook(m, 0x140, 0x40, can_read, can_write);
}

void mscan_reset(mcu_t *m)
{
	m->can0 = (mscan_t){ .tx_busy = -1 };
	m->can0.synch_due = 0;
	m->regs[CTL1] = LISTEN | INITAK;
	enter_init(m);
}
