/* Enhanced Capture Timer (S12ECT16B8CV1): free-running counter with prescaler, output compare
 * with pin actions (TCTL1/TCTL2, OC7M/OC7D, CFORC), input capture on PT edges (TCTL3/TCTL4,
 * latch mode without holding registers), fast flag clear (TFFCA) and the modulus down-counter.
 * Counters are computed from the time of their last clock edge rather than ticked. Pulse
 * accumulators, the input capture queue/latch modes and the delay counter are not modelled;
 * enabling them is logged. */
#include "mcu.h"

enum {
	TIOS = 0x40, CFORC = 0x41, OC7M = 0x42, OC7D = 0x43, TCNT = 0x44, TSCR1 = 0x46, TTOV = 0x47,
	TCTL1 = 0x48, TCTL2 = 0x49, TCTL3 = 0x4A, TCTL4 = 0x4B, TIE = 0x4C, TSCR2 = 0x4D, TFLG1 = 0x4E,
	TFLG2 = 0x4F, TC0 = 0x50, PACTL = 0x60, MCCTL = 0x66, MCFLG = 0x67, ICPAR = 0x68, DLYCT = 0x69,
	ICOVW = 0x6A, ICSYS = 0x6B, PTPSR = 0x6E, PBCTL = 0x70, MCCNT = 0x76,
};

#define TEN 0x80
#define TFFCA 0x10
#define PRNT 0x08

/* ---- free-running counter ---- */

static uint32_t timer_period(mcu_t *m)
{
	if (!(m->regs[TSCR1] & TEN))
		return 0;
	uint32_t pre = (m->regs[TSCR1] & PRNT) ? m->regs[PTPSR] + 1u : 1u << (m->regs[TSCR2] & 7);
	return m->bus_ticks * pre;
}

static uint16_t tcnt_now(mcu_t *m)
{
	ect_t *e = &m->ect;
	if (!e->tick_period)
		return e->tcnt_base;
	return (uint16_t)(e->tcnt_base + (m->now - e->tcnt_time) / e->tick_period);
}

/* Fold elapsed counts into the base so the period can change. */
static void tcnt_rebase(mcu_t *m)
{
	ect_t *e = &m->ect;
	if (e->tick_period) {
		uint64_t k = (m->now - e->tcnt_time) / e->tick_period;
		e->tcnt_base = (uint16_t)(e->tcnt_base + k);
		e->tcnt_time += k * e->tick_period;
	} else {
		e->tcnt_time = m->now;
	}
}

static void update_irqs(mcu_t *m)
{
	uint8_t f = m->regs[TFLG1] & m->regs[TIE];
	for (int n = 0; n < 8; n++)
		mcu_irq(m, (uint16_t)(0xFFEE - 2 * n), f >> n & 1);
	mcu_irq(m, 0xFFDE, (m->regs[TFLG2] & 0x80) && (m->regs[TSCR2] & 0x80));
	mcu_irq(m, 0xFFCA, (m->regs[MCFLG] & 0x80) && (m->regs[MCCTL] & 0x80));
}

/* Timer-driven pins of port T: channels in output compare with an OM/OL action, and channel-7
 * driven pins (OC7M). */
uint8_t ect_pt_override(mcu_t *m, uint8_t *mask)
{
	uint8_t tios = m->regs[TIOS], drive = 0;
	uint16_t ctl = (uint16_t)(m->regs[TCTL1] << 8 | m->regs[TCTL2]);
	for (int n = 0; n < 8; n++)
		if ((tios >> n & 1) && (ctl >> (2 * n) & 3))
			drive |= (uint8_t)(1 << n);
	if (tios & 0x80)
		drive |= m->regs[OC7M] & tios;
	*mask = drive;
	return m->ect.oc_level;
}

static void oc_action(mcu_t *m, int n)
{
	ect_t *e = &m->ect;
	uint16_t ctl = (uint16_t)(m->regs[TCTL1] << 8 | m->regs[TCTL2]);
	uint8_t bit = (uint8_t)(1 << n);
	switch (ctl >> (2 * n) & 3) {
	case 1: e->oc_level ^= bit; break;
	case 2: e->oc_level &= (uint8_t)~bit; break;
	case 3: e->oc_level |= bit; break;
	}
	if (n == 7) {   /* OC7M/OC7D: channel 7 also drives the masked pins */
		uint8_t mk = m->regs[OC7M] & m->regs[TIOS];
		e->oc_level = (uint8_t)((e->oc_level & ~mk) | (m->regs[OC7D] & mk));
	}
}

/* Process every compare and overflow up to now, then schedule the next one. */
void ect_event(mcu_t *m)
{
	ect_t *e = &m->ect;
	uint32_t p = e->tick_period;
	bool pins = false;
	if (p) {
		for (;;) {
			uint16_t cnt = e->tcnt_base;
			/* earliest edge after tcnt_time at which something happens */
			uint32_t best = 0x10000 - cnt;                         /* overflow to 0 */
			for (int n = 0; n < 8; n++)
				if (m->regs[TIOS] >> n & 1) {
					uint32_t d = (uint16_t)(mcu_reg16(m, (uint16_t)(TC0 + 2 * n)) - cnt);
					if (!d)
						d = 0x10000;
					if (d < best)
						best = d;
				}
			uint64_t t = e->tcnt_time + (uint64_t)best * p;
			if (t > m->now) {
				mcu_schedule(m, EV_ECT, t);
				break;
			}
			e->tcnt_time = t;
			e->tcnt_base = (uint16_t)(cnt + best);
			uint16_t now_cnt = e->tcnt_base;
			if (now_cnt == 0) {
				m->regs[TFLG2] |= 0x80;                         /* TOF */
				e->oc_level ^= m->regs[TTOV] & m->regs[TIOS];  /* toggle on overflow */
				pins |= m->regs[TTOV] != 0;
			}
			for (int n = 0; n < 8; n++)
				if ((m->regs[TIOS] >> n & 1) && mcu_reg16(m, (uint16_t)(TC0 + 2 * n)) == now_cnt) {
					m->regs[TFLG1] |= (uint8_t)(1 << n);
					oc_action(m, n);
					pins = true;
					if (n == 7 && (m->regs[TSCR2] & 0x08)) {   /* TCRE: counter reset by OC7 */
						e->tcnt_base = 0;
					}
				}
		}
	} else {
		mcu_schedule(m, EV_ECT, UINT64_MAX);
	}
	update_irqs(m);
	if (pins)
		mcu_ports_update(m);
}

/* Reschedule after a register change: process what is due, re-derive the next event. */
static void resched(mcu_t *m)
{
	tcnt_rebase(m);
	m->ect.tick_period = timer_period(m);
	ect_event(m);
}

/* ---- input capture ---- */

void ect_pins_changed(mcu_t *m, uint8_t levels)
{
	ect_t *e = &m->ect;
	uint8_t changed = levels ^ e->ic_last;
	e->ic_last = levels;
	if (!changed || !(m->regs[TSCR1] & TEN))
		return;
	uint16_t edg = (uint16_t)(m->regs[TCTL3] << 8 | m->regs[TCTL4]);
	for (int n = 0; n < 8; n++) {
		if (!(changed >> n & 1) || (m->regs[TIOS] >> n & 1))
			continue;
		int mode = edg >> (2 * n) & 3;                         /* 1 rising, 2 falling, 3 any */
		bool rising = levels >> n & 1;
		if (mode == 3 || (mode == 1 && rising) || (mode == 2 && !rising)) {
			uint16_t v = tcnt_now(m);
			m->regs[TC0 + 2 * n] = (uint8_t)(v >> 8);
			m->regs[TC0 + 2 * n + 1] = (uint8_t)v;
			m->regs[TFLG1] |= (uint8_t)(1 << n);
		}
	}
	update_irqs(m);
}

/* ---- modulus down-counter ---- */

static uint32_t mdc_period(mcu_t *m)
{
	static const uint8_t pre[4] = { 1, 4, 8, 16 };
	if (!(m->regs[MCCTL] & 0x04))
		return 0;
	return m->bus_ticks * pre[m->regs[MCCTL] & 3];
}

static uint16_t mc_now(mcu_t *m)
{
	ect_t *e = &m->ect;
	if (!e->mc_period || !e->mc_count)
		return e->mc_count;
	uint64_t k = (m->now - e->mc_time) / e->mc_period;
	return k >= e->mc_count ? 0 : (uint16_t)(e->mc_count - k);
}

/* Underflows up to now (reload in modulus mode), then schedule the next. */
void ect_mdc_event(mcu_t *m)
{
	ect_t *e = &m->ect;
	uint32_t p = e->mc_period;
	while (p && e->mc_count) {
		uint64_t t = e->mc_time + (uint64_t)e->mc_count * p;
		if (t > m->now) {
			mcu_schedule(m, EV_MDC, t);
			update_irqs(m);
			return;
		}
		m->regs[MCFLG] |= 0x80;                                 /* MCZF */
		e->mc_time = t;
		e->mc_count = (m->regs[MCCTL] & 0x40) ? e->mc_load : 0; /* MODMC: reload */
	}
	if (p && !e->mc_count) {                                   /* stopped at zero */
		e->mc_time = m->now;
	}
	mcu_schedule(m, EV_MDC, UINT64_MAX);
	update_irqs(m);
}

static void mdc_rebase(mcu_t *m)
{
	ect_t *e = &m->ect;
	ect_mdc_event(m);
	e->mc_count = mc_now(m);
	if (e->mc_period) {
		uint64_t k = (m->now - e->mc_time) / e->mc_period;
		e->mc_time += k * e->mc_period;
	} else {
		e->mc_time = m->now;
	}
}

/* ---- registers ---- */

static void fast_clear_note(mcu_t *m, uint16_t off)
{
	if (!(m->regs[TSCR1] & TFFCA))
		return;
	if (off == TCNT || off == TCNT + 1)
		m->regs[TFLG2] &= 0x7F;
	else if (off == MCCNT || off == MCCNT + 1)
		m->regs[MCFLG] &= 0x7F;
	update_irqs(m);
}

static uint8_t ect_read(mcu_t *m, uint16_t off)
{
	ect_event(m);
	ect_mdc_event(m);
	switch (off) {
	case CFORC:
		return 0;
	case TCNT: case TCNT + 1: {
		uint16_t v = tcnt_now(m);
		fast_clear_note(m, off);
		return off == TCNT ? (uint8_t)(v >> 8) : (uint8_t)v;
	}
	case MCCNT: case MCCNT + 1: {
		uint16_t v = (m->regs[MCCTL] & 0x20) ? m->ect.mc_load : mc_now(m);   /* RDMCL */
		fast_clear_note(m, off);
		return off == MCCNT ? (uint8_t)(v >> 8) : (uint8_t)v;
	}
	case MCCTL:
		return m->regs[off] & 0xE7;                             /* ICLAT, FLMC read 0 */
	}
	if (off >= TC0 && off < TC0 + 16) {
		int n = (off - TC0) >> 1;
		if ((m->regs[TSCR1] & TFFCA) && !(m->regs[TIOS] >> n & 1)) {   /* input capture read */
			m->regs[TFLG1] &= (uint8_t)~(1 << n);
			update_irqs(m);
		}
	}
	return m->regs[off];
}

static void log_unmodelled(mcu_t *m, uint16_t off, uint8_t v, const char *what)
{
	if (v && m->log)
		fprintf(m->log, "[%.6f] ECT %s ($%03X=$%02X) not modelled, PC $%04X\n", mcu_seconds(m, m->now), what, off, v,
			m->cpu.insn_pc);
}

static void ect_write(mcu_t *m, uint16_t off, uint8_t v)
{
	ect_t *e = &m->ect;
	ect_event(m);
	ect_mdc_event(m);
	switch (off) {
	case CFORC:                                                /* force compare actions, no flags */
		for (int n = 0; n < 8; n++)
			if ((v >> n & 1) && (m->regs[TIOS] >> n & 1))
				oc_action(m, n);
		mcu_ports_update(m);
		return;
	case TCNT: case TCNT + 1:                                  /* writable in special modes only */
		fast_clear_note(m, off);
		return;
	case TFLG1: case TFLG2:
		m->regs[off] &= (uint8_t)~v;
		update_irqs(m);
		return;
	case MCFLG:
		m->regs[off] = (uint8_t)((m->regs[off] & ~(v & 0x80)) & 0x80);
		update_irqs(m);
		return;
	case MCCTL: {
		uint8_t old = m->regs[off];
		mdc_rebase(m);
		m->regs[off] = v & 0xE7;
		if (!(v & 0x04)) {
			e->mc_count = 0xFFFF;                              /* disabled: preset to $FFFF */
		} else if (!(old & 0x04)) {
			e->mc_time = m->now;                               /* enabled: prescaler starts */
		}
		if ((v & 0x08) && (v & 0x04)) {                        /* FLMC: load now, reset prescaler */
			e->mc_count = e->mc_load;
			e->mc_time = m->now;
		}
		if (v & 0x10)
			log_unmodelled(m, off, v, "ICLAT");
		e->mc_period = mdc_period(m);
		ect_mdc_event(m);
		return;
	}
	case MCCNT:
		m->regs[off] = v;                                      /* high byte: completes with the low byte */
		fast_clear_note(m, off);
		return;
	case MCCNT + 1: {
		uint16_t w = (uint16_t)(m->regs[MCCNT] << 8 | v);
		fast_clear_note(m, off);
		mdc_rebase(m);
		e->mc_load = w;
		if (!(m->regs[MCCTL] & 0x40)) {                        /* not modulus mode: count from w now */
			e->mc_count = w;
			e->mc_time = m->now;
		}
		e->mc_period = mdc_period(m);
		ect_mdc_event(m);
		return;
	}
	case PACTL: case PBCTL: case ICSYS: case DLYCT: case ICOVW: case ICPAR:
		m->regs[off] = v;
		log_unmodelled(m, off, v, off == PACTL || off == PBCTL ? "pulse accumulator" : "input capture option");
		return;
	}
	m->regs[off] = v;
	if (off >= TC0 && off < TC0 + 16) {
		int n = (off - TC0) >> 1;
		if ((m->regs[TSCR1] & TFFCA) && (m->regs[TIOS] >> n & 1))   /* output compare write */
			m->regs[TFLG1] &= (uint8_t)~(1 << n);
	}
	if (off == TIOS || off == TCTL1 || off == TCTL2 || off == OC7M)
		mcu_ports_update(m);
	if (off == TIE || off == TSCR2 || off == TFLG1)
		update_irqs(m);
	resched(m);
}

void ect_init(mcu_t *m)
{
	mcu_hook(m, 0x40, 0x40, ect_read, ect_write);
}

void ect_reset(mcu_t *m)
{
	ect_t *e = &m->ect;
	*e = (ect_t){ 0 };
	e->tcnt_time = m->now;
	e->mc_count = 0xFFFF;
	e->mc_load = 0xFFFF;
	e->mc_time = m->now;
	m->regs[MCCNT] = 0xFF;
	m->regs[MCCNT + 1] = 0xFF;
	mcu_schedule(m, EV_ECT, UINT64_MAX);
	mcu_schedule(m, EV_MDC, UINT64_MAX);
}
