/* SPI0 (S12SPIV2), master mode: one transmit buffer plus the shifter, SPIF/SPTEF/MODF flags with
 * their read-sequence clearing, baud rate from SPI0BR. Bytes go to the board when they start
 * shifting (the board samples its chip selects then) and come back as the received byte when
 * the transfer ends.
 *
 * SPIDR writes: the block guide says a write before reading SPISR with SPTEF set is ignored, but
 * the MS1 firmware starts its display pump with a bare CLR of SPI0DR after init and the real
 * display runs, so a write is accepted whenever the transmit buffer is empty. */
#include "mcu.h"

enum { SPICR1 = 0xD8, SPICR2 = 0xD9, SPIBR = 0xDA, SPISR = 0xDB, SPIDR = 0xDD };
enum { SPIF = 0x80, SPTEF = 0x20, MODF = 0x10 };

static uint64_t byte_ticks(mcu_t *m)
{
	uint8_t br = m->regs[SPIBR];
	uint32_t div = (((br >> 4) & 7) + 1u) << ((br & 7) + 1);
	return mcu_bus_cycles(m, 8ull * div);
}

static void update_irq(mcu_t *m)
{
	uint8_t cr1 = m->regs[SPICR1], sr = m->regs[SPISR];
	bool on = ((cr1 & 0x80) && (sr & (SPIF | MODF))) || ((cr1 & 0x20) && (sr & SPTEF));
	mcu_irq(m, 0xFFD8, on && (cr1 & 0x40));
}

static void start(mcu_t *m, uint8_t v)
{
	spi_t *s = &m->spi0;
	s->busy = true;
	s->shift_out = v;
	uint8_t in = m->board.spi0_xfer ? m->board.spi0_xfer(m->board.ctx, v) : 0xFF;
	s->rx_pending = in;   /* held until the transfer ends */
	mcu_schedule(m, EV_SPI0, m->now + byte_ticks(m));
}

void spi_event(mcu_t *m)
{
	spi_t *s = &m->spi0;
	if (!s->busy)
		return;
	s->busy = false;
	if (!(m->regs[SPISR] & SPIF)) {   /* an unserviced SPIF keeps the first byte */
		s->rx = s->rx_pending;
		m->regs[SPISR] |= SPIF;
	}
	if (s->tx_full) {
		s->tx_full = false;
		m->regs[SPISR] |= SPTEF;
		start(m, s->shift_out_next);
	}
	update_irq(m);
}

static uint8_t spi_read(mcu_t *m, uint16_t off)
{
	spi_t *s = &m->spi0;
	switch (off) {
	case SPISR:
		s->sr_read_spif = m->regs[SPISR] & SPIF;
		return m->regs[SPISR];
	case SPIDR:
		if (s->sr_read_spif) {
			m->regs[SPISR] &= (uint8_t)~SPIF;
			s->sr_read_spif = false;
			update_irq(m);
		}
		return s->rx;
	}
	return m->regs[off];
}

static void spi_write(mcu_t *m, uint16_t off, uint8_t v)
{
	spi_t *s = &m->spi0;
	switch (off) {
	case SPISR:
		return;
	case SPIDR:
		if (!(m->regs[SPICR1] & 0x40) || s->tx_full)
			return;                       /* disabled, or buffer full: ignored */
		if (!s->busy) {
			start(m, v);                  /* to the shifter at once: SPTEF stays set */
		} else {
			s->tx_full = true;
			s->shift_out_next = v;
			m->regs[SPISR] &= (uint8_t)~SPTEF;
		}
		update_irq(m);
		return;
	case SPICR1:
		m->regs[off] = v;
		if (!(v & 0x40)) {                /* SPE cleared: abort, flags to reset state */
			s->busy = s->tx_full = false;
			m->regs[SPISR] = SPTEF;
			mcu_schedule(m, EV_SPI0, UINT64_MAX);
		}
		if ((v & 0x10) == 0 && (v & 0x40) && m->log)
			fprintf(m->log, "[%.6f] SPI0 slave mode not modelled\n", mcu_seconds(m, m->now));
		update_irq(m);
		return;
	}
	m->regs[off] = v;
}

void spi_init(mcu_t *m)
{
	mcu_hook(m, 0xD8, 8, spi_read, spi_write);
}

void spi_reset(mcu_t *m)
{
	m->spi0 = (spi_t){ 0 };
	m->regs[SPICR1] = 0x04;
	m->regs[SPISR] = SPTEF;
	mcu_schedule(m, EV_SPI0, UINT64_MAX);
	update_irq(m);
}
