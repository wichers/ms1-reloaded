/* MC9S12D64 core: memory map (S12MMCV4), interrupts (S12INTV1), clocks (S12CRGV4), ports
 * (S12MEBIV3, PIM), event scheduling and the run loop. */
#include "mcu.h"

#include <stdlib.h>
#include <string.h>

/* ---- time ---- */

double mcu_seconds(const mcu_t *m, uint64_t ticks)
{
	return (double)ticks / ((double)m->osc_hz * TICKS_PER_OSC);
}

uint64_t mcu_ticks(const mcu_t *m, double seconds)
{
	return (uint64_t)(seconds * (double)m->osc_hz * TICKS_PER_OSC + 0.5);
}

void mcu_schedule(mcu_t *m, int ev, uint64_t when)
{
	m->ev_when[ev] = when;
	uint64_t next = UINT64_MAX;
	for (int i = 0; i < EV_COUNT; i++)
		if (m->ev_when[i] < next)
			next = m->ev_when[i];
	m->next_event = next;
}

/* ---- interrupts ---- */

void mcu_irq(mcu_t *m, uint16_t vector, bool level)
{
	uint64_t bit = 1ull << ((vector - 0xFF80) >> 1);
	if (level)
		m->irq_pending |= bit;
	else
		m->irq_pending &= ~bit;
}

/* Highest-priority pending I-maskable interrupt: the HPRIO vector if pending, else the highest
 * vector address (Table 5-1 order). */
static uint16_t irq_vector(void *ctx)
{
	mcu_t *m = ctx;
	uint64_t p = m->irq_pending;
	if (!p || m->irq_block)
		return 0;
	uint16_t hprio = 0xFF00 | m->regs[R_HPRIO];
	if (hprio >= 0xFF80 && (p >> ((hprio - 0xFF80) >> 1) & 1))
		return hprio;
	return (uint16_t)(0xFF80 + 2 * (63 - __builtin_clzll(p)));
}

static bool xirq(void *ctx)
{
	return ((mcu_t *)ctx)->xirq_pin && !((mcu_t *)ctx)->irq_block;
}

/* ---- registers ---- */

static void reg_note(mcu_t *m, uint16_t off, int how, uint8_t v)
{
	if (m->reg_plain[off] || (m->reg_seen[off] & how))
		return;
	m->reg_seen[off] |= (uint8_t)how;
	if (m->log)
		fprintf(m->log, "[%.6f] %s unmodelled register $%03X (%s $%02X) at PC $%04X\n", mcu_seconds(m, m->now),
			how == 1 ? "read of" : "write to", off, how == 1 ? "has" : "value", v, m->cpu.insn_pc);
}

void mcu_hook(mcu_t *m, uint16_t off, int count, reg_rd_t rd, reg_wr_t wr)
{
	for (int i = 0; i < count; i++) {
		m->rd[off + i] = rd;
		m->wr[off + i] = wr;
		m->reg_plain[off + i] = true;
	}
}

void mcu_plain(mcu_t *m, uint16_t off, int count)
{
	for (int i = 0; i < count; i++)
		m->reg_plain[off + i] = true;
}

static uint8_t reg_read(mcu_t *m, uint16_t off)
{
	if (m->rd[off])
		return m->rd[off](m, off);
	reg_note(m, off, 1, m->regs[off]);
	return m->regs[off];
}

static void reg_write(mcu_t *m, uint16_t off, uint8_t v)
{
	if (m->wr[off]) {
		m->wr[off](m, off, v);
		return;
	}
	reg_note(m, off, 2, v);
	m->regs[off] = v;
}

uint16_t mcu_reg16(mcu_t *m, uint16_t off)
{
	return (uint16_t)(m->regs[off] << 8 | m->regs[off + 1]);
}

/* ---- memory map ---- */

static uint16_t reg_base(const mcu_t *m) { return (uint16_t)((m->regs[R_INITRG] & 0x78) << 8); }
static uint16_t ram_base(const mcu_t *m) { return (uint16_t)((m->regs[R_INITRM] & 0xF0) << 8); } /* 4K RAM: RAM15-RAM12 */
static uint16_t ee_base(const mcu_t *m) { return (uint16_t)((m->regs[R_INITEE] & 0xF8) << 8); }   /* 2K space, 1K twice */

static const uint8_t *flash_at(const mcu_t *m, uint16_t addr)
{
	if (addr >= 0xC000)
		return &m->flash[3][addr - 0xC000];
	if (addr >= 0x8000)  /* 64K part: PPAGE $38-$3F alias onto $3C-$3F */
		return &m->flash[m->regs[R_PPAGE] & 3][addr - 0x8000];
	if (addr >= 0x4000)
		return &m->flash[2][addr - 0x4000];
	return 0;
}

static void unmapped(mcu_t *m, uint16_t addr, const char *what)
{
	static uint16_t last;
	if (m->log && addr != last)
		fprintf(m->log, "[%.6f] %s unmapped address $%04X at PC $%04X\n", mcu_seconds(m, m->now), what, addr,
			m->cpu.insn_pc);
	last = addr;
}

uint8_t mcu_read(mcu_t *m, uint16_t addr)
{
	uint16_t off = (uint16_t)(addr - reg_base(m));
	if (off < REG_SIZE)
		return reg_read(m, off);
	off = (uint16_t)(addr - ram_base(m));
	if (off < RAM_SIZE)
		return m->ram[off];
	off = (uint16_t)(addr - ee_base(m));
	if ((m->regs[R_INITEE] & 1) && off < 2 * EEPROM_SIZE)
		return m->eeprom[off & (EEPROM_SIZE - 1)];
	const uint8_t *f = flash_at(m, addr);
	if (f)
		return *f;
	unmapped(m, addr, "read of");
	return 0;
}

uint8_t mcu_peek(mcu_t *m, uint16_t addr)
{
	uint16_t off = (uint16_t)(addr - reg_base(m));
	if (off < REG_SIZE)
		return m->regs[off];
	return mcu_read(m, addr);
}

void mcu_write(mcu_t *m, uint16_t addr, uint8_t v)
{
	if (m->write_hook)
		m->write_hook(m, addr, v);
	uint16_t off = (uint16_t)(addr - reg_base(m));
	if (off < REG_SIZE) {
		reg_write(m, off, v);
		return;
	}
	off = (uint16_t)(addr - ram_base(m));
	if (off < RAM_SIZE) {
		m->ram[off] = v;
		return;
	}
	off = (uint16_t)(addr - ee_base(m));
	if ((m->regs[R_INITEE] & 1) && off < 2 * EEPROM_SIZE) {
		if (!eeprom_array_write(m, off & (EEPROM_SIZE - 1), v) && m->log)
			fprintf(m->log, "[%.6f] EEPROM write $%04X=$%02X outside a command sequence at PC $%04X\n",
				mcu_seconds(m, m->now), addr, v, m->cpu.insn_pc);
		return;
	}
	if (flash_at(m, addr)) {
		if (m->log)
			fprintf(m->log, "[%.6f] flash write $%04X=$%02X at PC $%04X (flash programming not modelled)\n",
				mcu_seconds(m, m->now), addr, v, m->cpu.insn_pc);
		return;
	}
	unmapped(m, addr, "write to");
}

static uint8_t bus_read(void *ctx, uint16_t a) { return mcu_read(ctx, a); }
static void bus_write(void *ctx, uint16_t a, uint8_t v) { mcu_write(ctx, a, v); }
static uint8_t get_ppage(void *ctx) { return ((mcu_t *)ctx)->regs[R_PPAGE]; }
static void set_ppage(void *ctx, uint8_t p) { ((mcu_t *)ctx)->regs[R_PPAGE] = p & 0x3F; }

/* ---- MMC ---- */

static void mmc_write(mcu_t *m, uint16_t off, uint8_t v)
{
	/* INITRM, INITRG and MISC are write-once in normal modes; INITEE's EEON always writable */
	int bit = off - R_INITRM;
	switch (off) {
	case R_INITRM: v &= 0xF9; break;
	case R_INITRG: v &= 0x78; break;
	case R_INITEE: v &= 0xF9; break;
	case R_MISC: v &= 0x0F; break;
	case R_PPAGE: m->regs[off] = v & 0x3F; return;
	default: return;
	}
	if (off == R_INITEE) {
		m->regs[off] = v;
		return;
	}
	if (m->written_once & (1 << bit)) {
		if (m->log && m->regs[off] != v)
			fprintf(m->log, "[%.6f] write-once register $%03X already set to $%02X: write of $%02X ignored (PC $%04X)\n",
				mcu_seconds(m, m->now), off, m->regs[off], v, m->cpu.insn_pc);
		return;
	}
	m->written_once |= (uint8_t)(1 << bit);
	m->regs[off] = v;
}

/* ---- CRG ---- */

static void update_bus_clock(mcu_t *m)
{
	uint32_t ticks = 2 * TICKS_PER_OSC;                 /* bus = OSCCLK / 2 */
	if (m->regs[R_CLKSEL] & 0x80) {                     /* bus = PLLCLK / 2 = OSC (SYNR+1)/(REFDV+1) */
		uint32_t num = TICKS_PER_OSC * ((m->regs[R_REFDV] & 0x0F) + 1u);
		uint32_t den = (m->regs[R_SYNR] & 0x3F) + 1u;
		ticks = num / den;
		if (num % den && m->log)
			fprintf(m->log, "PLL bus period is not a whole number of ticks (SYNR=%u REFDV=%u): rounded\n",
				m->regs[R_SYNR], m->regs[R_REFDV]);
		if (!ticks)
			ticks = 1;
	}
	if (ticks != m->bus_ticks) {
		/* bring bus-clocked counters up to date at the old rate, then reschedule at the new */
		ect_event(m);
		ect_mdc_event(m);
		m->bus_ticks = ticks;
		ect_event(m);
		ect_mdc_event(m);
	}
}

static void crg_relock(mcu_t *m)
{
	m->regs[R_CRGFLG] &= (uint8_t)~0x0C;                /* LOCK, TRACK */
	if (m->regs[R_PLLCTL] & 0x40)                       /* PLLON */
		mcu_schedule(m, EV_PLL, m->now + mcu_ticks(m, 0.0005));
}

static void crg_write(mcu_t *m, uint16_t off, uint8_t v)
{
	switch (off) {
	case R_SYNR:
		m->regs[off] = v & 0x3F;
		crg_relock(m);
		break;
	case R_REFDV:
		m->regs[off] = v & 0x0F;
		crg_relock(m);
		break;
	case R_CRGFLG:                                      /* write 1 to clear RTIF PORF LVRF LOCKIF SCMIF */
		m->regs[off] &= (uint8_t)~(v & 0xF2);
		mcu_irq(m, 0xFFC6, (m->regs[R_CRGFLG] & 0x10) && (m->regs[R_CRGINT] & 0x10));
		break;
	case R_CLKSEL:
		/* PLLSEL cannot be set while the PLL is not locked (AUTO) or tracking */
		if ((v & 0x80) && !(m->regs[R_CRGFLG] & 0x08)) {
			v &= 0x7F;
			if (m->log)
				fprintf(m->log, "[%.6f] PLLSEL write ignored: PLL not locked (PC $%04X)\n", mcu_seconds(m, m->now),
					m->cpu.insn_pc);
		}
		m->regs[off] = v;
		update_bus_clock(m);
		break;
	case R_PLLCTL:
		m->regs[off] = v;
		if (!(v & 0x40))
			m->regs[R_CRGFLG] &= (uint8_t)~0x0C;
		break;
	case R_COPCTL:
		m->regs[off] = v;
		if ((v & 7) && m->log)
			fprintf(m->log, "[%.6f] COP enabled (COPCTL=$%02X): the watchdog is not modelled\n", mcu_seconds(m, m->now), v);
		break;
	case R_RTICTL:
		m->regs[off] = v;
		if (v && m->log)
			fprintf(m->log, "[%.6f] RTI enabled (RTICTL=$%02X): not modelled\n", mcu_seconds(m, m->now), v);
		break;
	default:
		m->regs[off] = v;
	}
}

static void pll_event(mcu_t *m)
{
	mcu_schedule(m, EV_PLL, UINT64_MAX);
	m->regs[R_CRGFLG] |= 0x0C | 0x10;                   /* LOCK, TRACK, LOCKIF */
	mcu_irq(m, 0xFFC6, m->regs[R_CRGINT] & 0x10);
}

/* ---- ports ---- */

typedef struct {
	uint16_t data, ddr, input;    /* input: PTIx register, 0 if none */
} port_def_t;

static const port_def_t ports[PORT_COUNT] = {
	[PORT_A] = { R_PORTA, R_DDRA, 0 },
	[PORT_B] = { R_PORTB, R_DDRB, 0 },
	[PORT_E] = { R_PORTE, R_DDRE, 0 },
	[PORT_K] = { R_PORTK, R_DDRK, 0 },
	[PORT_T] = { R_PTT, R_DDRT, R_PTIT },
	[PORT_S] = { R_PTS, R_PTS + 2, R_PTS + 1 },
	[PORT_M] = { R_PTM, R_DDRM, R_PTIM },
	[PORT_P] = { R_PTP, R_PTP + 2, R_PTP + 1 },
	[PORT_H] = { R_PTH, R_PTH + 2, R_PTH + 1 },
	[PORT_J] = { R_PTJ, R_PTJ + 2, R_PTJ + 1 },
	[PORT_AD] = { 0x8F, 0, 0 },   /* PORTAD0: input only, ATDDIEN enables the digital buffer */
};

static uint8_t pins_in(mcu_t *m, int port)
{
	return m->board.pins_in ? m->board.pins_in(m->board.ctx, port) : 0xFF;
}

/* Pin levels as the MCU sees them: driven bits from the port (or the timer), the rest from
 * outside. */
uint8_t mcu_port_pins(mcu_t *m, int port)
{
	const port_def_t *p = &ports[port];
	uint8_t ddr = p->ddr ? m->regs[p->ddr] : 0;
	uint8_t level = (uint8_t)((m->regs[p->data] & ddr) | (pins_in(m, port) & ~ddr));
	if (port == PORT_T) {
		uint8_t mask;
		uint8_t oc = ect_pt_override(m, &mask);
		level = (uint8_t)((level & ~mask) | (oc & mask));
	}
	return level;
}

static void port_update(mcu_t *m, int port)
{
	const port_def_t *p = &ports[port];
	uint8_t driven = p->ddr ? m->regs[p->ddr] : 0;
	uint8_t level = (uint8_t)(m->regs[p->data] & driven);
	if (port == PORT_T) {
		uint8_t mask;
		uint8_t oc = ect_pt_override(m, &mask);
		driven |= mask;
		level = (uint8_t)((level & ~mask) | (oc & mask));
	}
	if (m->pins_reported[port][0] == level && m->pins_reported[port][1] == driven)
		return;
	m->pins_reported[port][0] = level;
	m->pins_reported[port][1] = driven;
	if (m->board.pins_out)
		m->board.pins_out(m->board.ctx, port, level, driven);
}

void mcu_ports_update(mcu_t *m)
{
	for (int i = 0; i < PORT_COUNT; i++)
		port_update(m, i);
}

static int port_of(uint16_t off)
{
	for (int i = 0; i < PORT_COUNT; i++)
		if (ports[i].data == off || (ports[i].ddr && ports[i].ddr == off) || (ports[i].input && ports[i].input == off))
			return i;
	return -1;
}

static uint8_t port_read(mcu_t *m, uint16_t off)
{
	int port = port_of(off);
	const port_def_t *p = &ports[port];
	if (off == p->ddr)
		return m->regs[off];
	uint8_t pins = mcu_port_pins(m, port);
	if (off == p->input && p->input)
		return pins;
	uint8_t ddr = p->ddr ? m->regs[p->ddr] : 0;
	return (uint8_t)((m->regs[off] & ddr) | (pins & ~ddr));
}

static void port_write(mcu_t *m, uint16_t off, uint8_t v)
{
	int port = port_of(off);
	if (off == ports[port].input)
		return;
	m->regs[off] = v;
	port_update(m, port);
	if (port == PORT_T)
		ect_pins_changed(m, mcu_port_pins(m, PORT_T));
}

/* ---- init / reset / run ---- */

void mcu_init(mcu_t *m, uint32_t osc_hz, FILE *log)
{
	memset(m, 0, sizeof *m);
	m->osc_hz = osc_hz;
	m->log = log;
	m->cpu.bus = (cpu12_bus_t){ m, bus_read, bus_write, irq_vector, xirq, get_ppage, set_ppage };
	memset(m->flash, 0xFF, sizeof m->flash);
	memset(m->eeprom, 0xFF, sizeof m->eeprom);

	mcu_hook(m, R_INITRM, 4, 0, mmc_write);
	mcu_hook(m, R_PPAGE, 1, 0, mmc_write);
	mcu_hook(m, R_SYNR, 2, 0, crg_write);
	mcu_hook(m, R_CRGFLG, 6, 0, crg_write);              /* CRGFLG..COPCTL */
	mcu_hook(m, R_ARMCOP, 1, 0, crg_write);
	for (int i = 0; i < PORT_COUNT; i++) {
		const port_def_t *p = &ports[i];
		mcu_hook(m, p->data, 1, port_read, port_write);
		if (p->ddr)
			mcu_hook(m, p->ddr, 1, port_read, port_write);
		if (p->input)
			mcu_hook(m, p->input, 1, port_read, port_write);
	}
	/* plain storage: MEBI/PIM configuration, INT, IDs, MEMSIZ, BKP */
	mcu_plain(m, 0x04, 4);                               /* PORTC/D, DDRC/D: reserved in single-chip mode */
	mcu_plain(m, R_PEAR, 5);                             /* PEAR MODE PUCR RDRIV EBICTL */
	mcu_plain(m, 0x14, 0x0C);                            /* MTST0, ITCR, ITEST, ..., PARTID, MEMSIZ, INTCR, HPRIO */
	mcu_plain(m, 0x20, 0x10);
	mcu_plain(m, 0x31, 1);
	mcu_plain(m, R_PTT + 3, 3);                          /* RDRT PERT PPST */
	mcu_plain(m, R_PTS + 3, 5);
	mcu_plain(m, R_PTM + 3, 5);                          /* RDRM PERM PPSM WOMM MODRR */
	mcu_plain(m, R_PTP + 3, 5);
	mcu_plain(m, R_PTH + 3, 5);
	mcu_plain(m, R_PTJ + 3, 5);

	ect_init(m);
	spi_init(m);
	eeprom_init(m);
	atd_init(m);
	mscan_init(m);
}

void mcu_reset(mcu_t *m)
{
	memset(m->regs, 0, sizeof m->regs);
	m->written_once = 0;
	m->irq_pending = 0;
	for (int i = 0; i < EV_COUNT; i++)
		m->ev_when[i] = UINT64_MAX;
	m->next_event = UINT64_MAX;
	memset(m->pins_reported, 0xFF, sizeof m->pins_reported);

	/* reset values: Device User Guide section 1 and the block guides */
	m->regs[R_INITRM] = 0x09;
	m->regs[R_INITEE] = 0x01;
	m->regs[R_MISC] = 0x0D;                              /* normal single chip, ROMON */
	m->regs[R_MODE] = 0x80;                              /* normal single chip */
	m->regs[R_PUCR] = 0x90;
	m->regs[R_PEAR] = 0x10;
	m->regs[0x1A] = 0x03;                                /* PARTID: MC9S12D64 mask set family */
	m->regs[0x1C] = 0x25;                                /* MEMSIZ0: 2K EEPROM space, 4K RAM */
	m->regs[0x1D] = 0x80;                                /* MEMSIZ1: 64K flash */
	m->regs[R_INTCR] = 0x40;                             /* IRQEN */
	m->regs[R_HPRIO] = 0xF2;
	m->regs[R_CRGFLG] = 0x40;                            /* PORF */
	m->regs[R_PLLCTL] = 0xF1;
	m->bus_ticks = 2 * TICKS_PER_OSC;
	crg_relock(m);

	ect_reset(m);
	spi_reset(m);
	eeprom_reset(m);
	atd_reset(m);
	mscan_reset(m);
	mcu_ports_update(m);
	cpu12_reset(&m->cpu, VEC_RESET);
}

static void fire_events(mcu_t *m)
{
	while (m->next_event <= m->now) {
		int ev = 0;
		for (int i = 1; i < EV_COUNT; i++)
			if (m->ev_when[i] < m->ev_when[ev])
				ev = i;
		mcu_schedule(m, ev, UINT64_MAX);
		switch (ev) {
		case EV_PLL: pll_event(m); break;
		case EV_ECT: ect_event(m); break;
		case EV_MDC: ect_mdc_event(m); break;
		case EV_SPI0: spi_event(m); break;
		case EV_ATD0: atd_event(m); break;
		case EV_EEPROM: eeprom_event(m); break;
		case EV_CAN0: mscan_event(m); break;
		case EV_BOARD: if (m->board.event) m->board.event(m->board.ctx); break;
		}
	}
}

uint64_t mcu_run(mcu_t *m, uint64_t until)
{
	cpu12_t *c = &m->cpu;
	bool resume = true;        /* do not stop again at the breakpoint we stopped at */
	m->stop = false;
	m->stop_reason = 0;
	while (m->now < until && !m->stop) {
		fire_events(m);
		/* next_event can move earlier while instructions run (a register write schedules) */
		while (m->now < until && m->now < m->next_event) {
			if (m->breakpoints && !resume && c->state == CPU12_RUN &&
			    (m->breakpoints[c->pc >> 3] >> (c->pc & 7) & 1)) {
				m->stop = true;
				m->stop_reason = "breakpoint";
				return m->now;
			}
			resume = false;
			uint16_t pc = c->pc;
			int cycles = cpu12_step(c);
			if (!cycles) {
				if (c->state == CPU12_HALT) {
					m->stop = true;
					m->stop_reason = c->halt_reason;
					return m->now;
				}
				m->now = m->next_event < until ? m->next_event : until;   /* waiting */
				break;
			}
			if (c->insn_pc == pc)
				m->trace[m->trace_pos++ & 4095] = pc;
			m->now += mcu_bus_cycles(m, (uint64_t)cycles);
			if (m->step_hook)
				m->step_hook(m, pc);
			if (m->stop)
				return m->now;
		}
	}
	fire_events(m);
	return m->now;
}
