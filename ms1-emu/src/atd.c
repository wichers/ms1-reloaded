/* ATD0 (S12ATD10B8CV2): conversion sequences of 1-8 conversions (ATDCTL3), single or
 * multi-channel (MULT), one-shot or continuous (SCAN), 8- or 10-bit, left/right justified,
 * unsigned/signed. One conversion takes 2 + sample (ATDCTL4 SMP) + 8 or 10 ATD clocks, the
 * ATD clock being bus / (2 (PRS + 1)). Results come from the board's analog inputs when each
 * conversion ends. Fast flag clear (AFFC) clears a CCF bit on its result read. External
 * triggers and the FIFO mode are not modelled. */
#include "mcu.h"

enum {
	ATDCTL2 = 0x82, ATDCTL3 = 0x83, ATDCTL4 = 0x84, ATDCTL5 = 0x85, ATDSTAT0 = 0x86, ATDSTAT1 = 0x8B,
	ATDDIEN = 0x8D, ATDDR0 = 0x90,
};

static int seq_len(mcu_t *m)
{
	int n = m->regs[ATDCTL3] >> 3 & 0x0F;
	return n == 0 || n > 8 ? 8 : n;
}

static uint64_t conv_ticks(mcu_t *m)
{
	uint8_t c4 = m->regs[ATDCTL4];
	static const int smp[4] = { 2, 4, 8, 16 };
	int clocks = 2 + smp[c4 >> 5 & 3] + ((c4 & 0x80) ? 8 : 10);
	return mcu_bus_cycles(m, (uint64_t)clocks * 2 * ((c4 & 0x1F) + 1));
}


static void update_irq(mcu_t *m)
{
	mcu_irq(m, 0xFFD2, (m->regs[ATDCTL2] & 0x02) && (m->regs[ATDCTL2] & 0x01));
}

static void start_seq(mcu_t *m)
{
	m->atd0.conv_index = 0;
	m->atd0.busy = true;
	mcu_schedule(m, EV_ATD0, m->now + conv_ticks(m));
}

static void store(mcu_t *m, int slot, int channel)
{
	uint16_t v = m->board.atd0_input ? m->board.atd0_input(m->board.ctx, channel) : 0;
	uint8_t c5 = m->regs[ATDCTL5];
	bool eight = m->regs[ATDCTL4] & 0x80;
	if (v > 0x3FF)
		v = 0x3FF;
	if (eight)
		v >>= 2;
	if (c5 & 0x40)                                    /* DSGN: signed (left-justified only) */
		v ^= eight ? 0x80 : 0x200;
	uint16_t r = (c5 & 0x80) ? v : (uint16_t)(v << (eight ? 8 : 6));   /* DJM */
	m->regs[ATDDR0 + 2 * slot] = (uint8_t)(r >> 8);
	m->regs[ATDDR0 + 2 * slot + 1] = (uint8_t)r;
	m->regs[ATDSTAT1] |= (uint8_t)(1 << slot);
}

void atd_event(mcu_t *m)
{
	if (!m->atd0.busy)
		return;
	uint8_t c5 = m->regs[ATDCTL5];
	int first = c5 & 7;
	int channel = (c5 & 0x10) ? (first + m->atd0.conv_index) & 7 : first;
	store(m, m->atd0.conv_index, channel);
	m->regs[ATDSTAT0] = (uint8_t)((m->regs[ATDSTAT0] & 0xF8) | ((m->atd0.conv_index + 1) & 7));
	if (++m->atd0.conv_index < seq_len(m)) {
		mcu_schedule(m, EV_ATD0, m->now + conv_ticks(m));
		return;
	}
	m->regs[ATDSTAT0] |= 0x80;                        /* SCF */
	m->regs[ATDCTL2] |= 0x01;                         /* ASCIF */
	update_irq(m);
	if (c5 & 0x20)                                    /* SCAN: continuous */
		start_seq(m);
	else
		m->atd0.busy = false;
}

static uint8_t atd_read(mcu_t *m, uint16_t off)
{
	if (off >= ATDDR0 && off < ATDDR0 + 16 && (m->regs[ATDCTL2] & 0x40)) {   /* AFFC */
		m->regs[ATDSTAT1] &= (uint8_t)~(1 << ((off - ATDDR0) >> 1));
		m->regs[ATDSTAT0] &= 0x7F;
		m->regs[ATDCTL2] &= 0xFE;
		update_irq(m);
	}
	return m->regs[off];
}

static void atd_write(mcu_t *m, uint16_t off, uint8_t v)
{
	switch (off) {
	case ATDCTL2:
		m->regs[off] = (uint8_t)((v & 0xFE) | (m->regs[off] & 0x01));
		if ((v & 0x1C) && m->log)
			fprintf(m->log, "[%.6f] ATD0 external trigger not modelled\n", mcu_seconds(m, m->now));
		if (!(v & 0x80)) {                            /* ADPU off */
			m->atd0.busy = false;
			mcu_schedule(m, EV_ATD0, UINT64_MAX);
		}
		update_irq(m);
		return;
	case ATDCTL5:                                     /* starts a new sequence, aborting one running */
		m->regs[off] = v;
		m->regs[ATDSTAT0] &= 0x78;
		m->regs[ATDCTL2] &= 0xFE;
		m->regs[ATDSTAT1] = 0;
		if (m->regs[ATDCTL2] & 0x80)
			start_seq(m);
		update_irq(m);
		return;
	case ATDSTAT0:                                    /* write 1 to clear SCF, ETORF, FIFOR */
		m->regs[off] &= (uint8_t)~(v & 0xB0);
		return;
	case ATDSTAT1:
		return;
	}
	if (off >= ATDDR0)
		return;
	m->regs[off] = v;
	if (off == ATDCTL3 && (v & 0x04) && m->log)
		fprintf(m->log, "[%.6f] ATD0 FIFO mode not modelled\n", mcu_seconds(m, m->now));
}

void atd_init(mcu_t *m)
{
	mcu_hook(m, 0x80, 0x0F, atd_read, atd_write);      /* PORTAD0 (0x8F) is the port model's */
	mcu_hook(m, ATDDR0, 0x10, atd_read, atd_write);
}

void atd_reset(mcu_t *m)
{
	m->atd0 = (atd_t){ 0 };
	m->regs[ATDCTL3] = 0x20;
	m->regs[ATDCTL4] = 0x05;
	mcu_schedule(m, EV_ATD0, UINT64_MAX);
	update_irq(m);
}
