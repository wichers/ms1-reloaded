/* EEPROM controller (EETS, 1K array). A command is a word write to the array (latching the
 * address and data), a write to ECMD, then writing CBEIF to launch it. One command executes
 * while a second can be buffered (CBEIF set again once the first leaves the buffer); CCIF is set
 * when all have completed. Times (Device User Guide Table A-11 formulas): word program
 * 9/fNVMOP + 25/fbus, sector erase 4000/fNVMOP, mass erase 20000/fNVMOP, with fNVMOP from
 * ECLKDIV. Programming ANDs the data into the array, as on an unerased cell. */
#include <string.h>

#include "mcu.h"

enum { ECLKDIV = 0x110, ECNFG = 0x113, EPROT = 0x114, ESTAT = 0x115, ECMD = 0x116 };
enum { CBEIF = 0x80, CCIF = 0x40, PVIOL = 0x20, ACCERR = 0x10, BLANK = 0x04 };
enum { CMD_VERIFY = 0x05, CMD_PROGRAM = 0x20, CMD_ERASE = 0x40, CMD_MASS = 0x41, CMD_ABORT = 0x47, CMD_MODIFY = 0x60 };


static void update_irq(mcu_t *m)
{
	uint8_t st = m->regs[ESTAT], cfg = m->regs[ECNFG];
	mcu_irq(m, 0xFFBA, ((cfg & 0x80) && (st & CBEIF)) || ((cfg & 0x40) && (st & CCIF)));
}

static double fnvmop(mcu_t *m)
{
	uint8_t d = m->regs[ECLKDIV];
	return (double)m->osc_hz / (((d & 0x40) ? 8.0 : 1.0) * ((d & 0x3F) + 1));
}

static uint64_t duration(mcu_t *m, uint8_t cmd)
{
	double f = fnvmop(m), fbus = (double)m->osc_hz * TICKS_PER_OSC / m->bus_ticks;
	switch (cmd) {
	case CMD_PROGRAM: return mcu_ticks(m, 9 / f + 25 / fbus);
	case CMD_ERASE: return mcu_ticks(m, 4000 / f);
	case CMD_MODIFY: return mcu_ticks(m, 4000 / f + 9 / f + 25 / fbus);
	case CMD_MASS: return mcu_ticks(m, 20000 / f);
	default: return mcu_ticks(m, 50 / f);
	}
}

static void begin(mcu_t *m)
{
	mcu_schedule(m, EV_EEPROM, m->now + duration(m, m->ee.active.cmd));
}

/* The array is write-protected only by EPROT; the MS1 firmware leaves it open. */
static void execute(mcu_t *m, const eeprom_cmd_t *c)
{
	uint16_t a = c->addr & (EEPROM_SIZE - 2);
	switch (c->cmd) {
	case CMD_PROGRAM:
		m->eeprom[a] &= (uint8_t)(c->data >> 8);
		m->eeprom[a + 1] &= (uint8_t)c->data;
		break;
	case CMD_ERASE:
		memset(&m->eeprom[a & ~3], 0xFF, 4);
		break;
	case CMD_MODIFY:
		memset(&m->eeprom[a & ~3], 0xFF, 4);
		m->eeprom[a] = (uint8_t)(c->data >> 8);
		m->eeprom[a + 1] = (uint8_t)c->data;
		break;
	case CMD_MASS:
		memset(m->eeprom, 0xFF, EEPROM_SIZE);
		break;
	case CMD_VERIFY: {
		bool blank = true;
		for (int i = 0; i < EEPROM_SIZE; i++)
			blank &= m->eeprom[i] == 0xFF;
		if (blank)
			m->regs[ESTAT] |= BLANK;
		break;
	}
	}
}

void eeprom_event(mcu_t *m)
{
	if (!m->ee.has_active)
		return;
	execute(m, &m->ee.active);
	if (m->ee.has_queued) {
		m->ee.active = m->ee.queued;
		m->ee.has_queued = false;
		m->regs[ESTAT] |= CBEIF;
		begin(m);
	} else {
		m->ee.has_active = false;
		m->regs[ESTAT] |= CCIF;
	}
	update_irq(m);
}

/* Array write during a command sequence: latch the word; false if none is expected. */
bool eeprom_array_write(mcu_t *m, uint16_t index, uint8_t v)
{
	eeprom_t *e = &m->ee;
	if (!(m->regs[ESTAT] & CBEIF) || !(m->regs[ECLKDIV] & 0x80)) {
		m->regs[ESTAT] |= ACCERR;
		return false;
	}
	if (index & 1) {
		e->data = (uint16_t)((e->data & 0xFF00) | v);
	} else {
		e->addr = index;
		e->data = (uint16_t)((e->data & 0x00FF) | v << 8);
	}
	e->latched = true;
	return true;
}

static void ee_write(mcu_t *m, uint16_t off, uint8_t v)
{
	eeprom_t *e = &m->ee;
	switch (off) {
	case ECLKDIV:
		if (!(m->regs[off] & 0x80))                   /* write once: EDIVLD set by the write */
			m->regs[off] = (uint8_t)((v & 0x7F) | 0x80);
		return;
	case ECMD:
		e->cmd = v;
		m->regs[off] = v;
		return;
	case ESTAT:
		m->regs[off] &= (uint8_t)~(v & (PVIOL | ACCERR));
		if (v & CBEIF) {                              /* launch */
			if (!e->latched || !(m->regs[ESTAT] & CBEIF)) {
				m->regs[ESTAT] |= ACCERR;
				break;
			}
			eeprom_cmd_t c = { e->addr, e->data, e->cmd };
			e->latched = false;
			m->regs[ESTAT] &= (uint8_t)~(CBEIF | CCIF | BLANK);
			if (!m->ee.has_active) {
				m->ee.active = c;
				m->ee.has_active = true;
				m->regs[ESTAT] |= CBEIF;                  /* moves on at once, buffer free */
				begin(m);
			} else {
				m->ee.queued = c;
				m->ee.has_queued = true;
			}
		}
		break;
	default:
		m->regs[off] = v;
	}
	update_irq(m);
}

void eeprom_init(mcu_t *m)
{
	mcu_hook(m, ECLKDIV, 12, 0, ee_write);
}

void eeprom_reset(mcu_t *m)
{
	m->ee = (eeprom_t){ 0 };
	m->ee.has_active = m->ee.has_queued = false;
	m->regs[ESTAT] = CBEIF | CCIF;
	m->regs[EPROT] = 0xFF;
	mcu_schedule(m, EV_EEPROM, UINT64_MAX);
	update_irq(m);
}
