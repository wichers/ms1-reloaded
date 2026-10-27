/* Flash controller (FTS64K: the 64 KiB array, four 16 KiB pages). As the EEPROM controller
 * (eeprom.c): a command is a word write to the array, latching the address and data, a write
 * to FCMD, then writing CBEIF to FSTAT to launch it; one command executes while a second can be
 * buffered, CCIF is set when all have completed. An array write reaches the page it addresses
 * as the CPU sees it: $3E at 0x4000-0x7FFF, PPAGE's page in the window, $3F from 0xC000.
 * Sector erase clears 512 bytes. Times (Device User Guide A.3): word program 9/fNVMOP +
 * 25/fbus, sector erase 4000/fNVMOP, mass erase 20000/fNVMOP, with fNVMOP from FCLKDIV
 * (write-once). FPROT and FSEC come from the array at reset (0xFF0D, 0xFF0F); protection is
 * not modelled. The array is one block: a CPU read of it while a command runs stops the run
 * (mcu_read_flash_busy), as the part cannot deliver it. */
#include <string.h>

#include "mcu.h"

enum { FCLKDIV = 0x100, FSEC = 0x101, FCNFG = 0x103, FPROT = 0x104, FSTAT = 0x105, FCMD = 0x106 };
enum { CBEIF = 0x80, CCIF = 0x40, PVIOL = 0x20, ACCERR = 0x10, BLANK = 0x04 };
enum { CMD_VERIFY = 0x05, CMD_PROGRAM = 0x20, CMD_ERASE = 0x40, CMD_MASS = 0x41 };
#define SECTOR 512

static uint8_t *array(mcu_t *m) { return &m->flash[0][0]; }

static void update_irq(mcu_t *m)
{
	uint8_t st = m->regs[FSTAT], cfg = m->regs[FCNFG];
	mcu_irq(m, 0xFFB8, ((cfg & 0x80) && (st & CBEIF)) || ((cfg & 0x40) && (st & CCIF)));
}

static double fnvmop(mcu_t *m)
{
	uint8_t d = m->regs[FCLKDIV];
	return (double)m->osc_hz / (((d & 0x40) ? 8.0 : 1.0) * ((d & 0x3F) + 1));
}

static uint64_t duration(mcu_t *m, uint8_t cmd)
{
	double f = fnvmop(m), fbus = (double)m->osc_hz * TICKS_PER_OSC / m->bus_ticks;
	switch (cmd) {
	case CMD_PROGRAM: return mcu_ticks(m, 9 / f + 25 / fbus);
	case CMD_ERASE: return mcu_ticks(m, 4000 / f);
	case CMD_MASS: return mcu_ticks(m, 20000 / f);
	default: return mcu_ticks(m, 50 / f);
	}
}

static void begin(mcu_t *m)
{
	mcu_schedule(m, EV_FLASH, m->now + duration(m, m->fl.active.cmd));
}

static void execute(mcu_t *m, const eeprom_cmd_t *c)
{
	uint32_t a = c->addr & ~1u;
	uint8_t *f = array(m);
	switch (c->cmd) {
	case CMD_PROGRAM:
		if (f[a] != 0xFF || f[a + 1] != 0xFF) {
			m->regs[FSTAT] |= ACCERR;               /* a word programmed twice */
			if (m->log)
				fprintf(m->log, "[%.6f] flash word at page $%02X offset $%04X programmed again\n",
					mcu_seconds(m, m->now), FIRST_PAGE + a / PAGE_SIZE, a % PAGE_SIZE);
		}
		f[a] &= (uint8_t)(c->data >> 8);
		f[a + 1] &= (uint8_t)c->data;
		break;
	case CMD_ERASE:
		memset(&f[a & ~(uint32_t)(SECTOR - 1)], 0xFF, SECTOR);
		break;
	case CMD_MASS:
		memset(f, 0xFF, FLASH_PAGES * PAGE_SIZE);
		break;
	case CMD_VERIFY: {
		bool blank = true;
		for (uint32_t i = 0; i < FLASH_PAGES * PAGE_SIZE; i++)
			blank &= f[i] == 0xFF;
		if (blank)
			m->regs[FSTAT] |= BLANK;
		break;
	}
	}
}

void flash_event(mcu_t *m)
{
	if (!m->fl.has_active)
		return;
	execute(m, &m->fl.active);
	if (m->fl.has_queued) {
		m->fl.active = m->fl.queued;
		m->fl.has_queued = false;
		m->regs[FSTAT] |= CBEIF;
		begin(m);
	} else {
		m->fl.has_active = false;
		m->regs[FSTAT] |= CCIF;
	}
	update_irq(m);
}

bool flash_busy(const mcu_t *m)
{
	return m->fl.has_active;
}

/* Array write during a command sequence (index into the four pages): latch the word; false if
 * none is expected. */
bool flash_array_write(mcu_t *m, uint32_t index, uint8_t v)
{
	eeprom_t *e = &m->fl;
	if (!(m->regs[FSTAT] & CBEIF) || !(m->regs[FCLKDIV] & 0x80)) {
		m->regs[FSTAT] |= ACCERR;
		return false;
	}
	if (index & 1) {
		e->data = (uint16_t)((e->data & 0xFF00) | v);
	} else {
		e->addr = (uint16_t)index;
		e->data = (uint16_t)((e->data & 0x00FF) | v << 8);
	}
	e->latched = true;
	return true;
}

static void fl_write(mcu_t *m, uint16_t off, uint8_t v)
{
	eeprom_t *e = &m->fl;
	switch (off) {
	case FCLKDIV:
		if (!(m->regs[off] & 0x80))                   /* write once: FDIVLD set by the write */
			m->regs[off] = (uint8_t)((v & 0x7F) | 0x80);
		return;
	case FSEC:
		return;                                       /* read-only */
	case FCMD:
		e->cmd = v;
		m->regs[off] = v;
		return;
	case FSTAT:
		m->regs[off] &= (uint8_t)~(v & (PVIOL | ACCERR));
		if (v & CBEIF) {                              /* launch */
			if (!e->latched || !(m->regs[FSTAT] & CBEIF)) {
				m->regs[FSTAT] |= ACCERR;
				break;
			}
			eeprom_cmd_t c = { e->addr, e->data, e->cmd };
			e->latched = false;
			m->regs[FSTAT] &= (uint8_t)~(CBEIF | CCIF | BLANK);
			if (!e->has_active) {
				e->active = c;
				e->has_active = true;
				m->regs[FSTAT] |= CBEIF;                  /* moves on at once, buffer free */
				begin(m);
			} else {
				e->queued = c;
				e->has_queued = true;
			}
		}
		break;
	default:
		m->regs[off] = v;
	}
	update_irq(m);
}

void flash_init(mcu_t *m)
{
	mcu_hook(m, FCLKDIV, 12, 0, fl_write);
}

void flash_reset(mcu_t *m)
{
	m->fl = (eeprom_t){ 0 };
	m->regs[FCLKDIV] = 0;
	m->regs[FSTAT] = CBEIF | CCIF;
	m->regs[FPROT] = m->flash[FLASH_PAGES - 1][0xFF0D - 0xC000];
	m->regs[FSEC] = m->flash[FLASH_PAGES - 1][0xFF0F - 0xC000];
	mcu_schedule(m, EV_FLASH, UINT64_MAX);
	update_irq(m);
}
