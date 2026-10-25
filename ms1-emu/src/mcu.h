/* MC9S12D64 model: memory map (MMC), interrupt controller (INT), clocks (CRG), event scheduler
 * and the peripherals the MS1 firmware uses. References are to the MC9S12DJ64 Device User
 * Guide V01.20 and the S12 block guides in doc/. */
#ifndef MCU_H
#define MCU_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "cpu12.h"
#include "image.h"

#define REG_SIZE 0x400
#define RAM_SIZE 0x1000
#define EEPROM_SIZE 0x400

/* Time is counted in ticks of 1/(TICKS_PER_OSC * fosc): with a 4 MHz crystal, 48 MHz ticks, so
 * the oscillator (12), the reset bus clock (24) and the PLL bus clock of SYNR=23/REFDV=3 (2)
 * are whole numbers of ticks. */
#define TICKS_PER_OSC 12

/* Scheduled events, one slot each. */
enum {
	EV_PLL,      /* PLL lock */
	EV_ECT,      /* timer: next output compare or overflow */
	EV_MDC,      /* modulus down-counter underflow */
	EV_SPI0,     /* SPI0 transfer complete */
	EV_ATD0,     /* ATD0 sequence complete */
	EV_EEPROM,   /* EEPROM command complete */
	EV_CAN0,     /* MSCAN0: transmit/receive completion, mode change */
	EV_BOARD,    /* board model */
	EV_COUNT
};

/* Ports, for the board interface. */
enum { PORT_A, PORT_B, PORT_E, PORT_K, PORT_T, PORT_S, PORT_M, PORT_P, PORT_H, PORT_J, PORT_AD, PORT_COUNT };

typedef struct mcu mcu_t;
typedef uint8_t (*reg_rd_t)(mcu_t *m, uint16_t off);
typedef void (*reg_wr_t)(mcu_t *m, uint16_t off, uint8_t v);

/* What the MCU needs from the board around it. Every callback may be NULL. */
typedef struct board_if {
	void *ctx;
	/* Level the outside world drives on a port's pins (bits where nothing drives read as pulled
	 * up or low, see the port model). */
	uint8_t (*pins_in)(void *ctx, int port);
	/* The MCU's driven pin levels changed: level for every pin, driven = output mask. */
	void (*pins_out)(void *ctx, int port, uint8_t level, uint8_t driven);
	/* SPI0 shifted a byte out; returns the byte shifted in (MISO). */
	uint8_t (*spi0_xfer)(void *ctx, uint8_t out);
	/* MSCAN0 put a frame on the bus (idr: 4 identifier register bytes, dlc, data). */
	void (*can0_tx)(void *ctx, const uint8_t idr[4], uint8_t dlc, const uint8_t *data);
	/* Analog input channel n of ATD0, 10-bit result. */
	uint16_t (*atd0_input)(void *ctx, int channel);
	/* EV_BOARD fired. */
	void (*event)(void *ctx);
} board_if_t;

typedef struct ect {
	uint16_t tcnt_base;      /* TCNT at tcnt_time */
	uint64_t tcnt_time;      /* tick of the last rebase (a timer clock edge) */
	uint32_t tick_period;    /* ticks per TCNT increment; 0 when stopped */
	uint8_t oc_level;        /* output-compare pin levels, channels 0-7 */
	uint16_t mc_count;       /* modulus counter at mc_time */
	uint64_t mc_time;
	uint32_t mc_period;      /* ticks per decrement; 0 when stopped */
	uint16_t mc_load;
	uint8_t ic_last;         /* last PT input levels seen, for edge detection */
} ect_t;

typedef struct spi {
	bool busy;               /* a byte is shifting */
	uint8_t shift_out;
	uint8_t rx_pending;      /* byte received for the transfer in progress */
	uint8_t rx;              /* SPIDR read value */
	bool tx_full;            /* SPIDR written while busy */
	uint8_t shift_out_next;
	bool sr_read_spif;       /* SPISR read with SPIF set: SPIDR read clears it */
} spi_t;

typedef struct eeprom_cmd { uint16_t addr, data; uint8_t cmd; } eeprom_cmd_t;

typedef struct eeprom {
	bool latched;            /* an array write latched address/data for a command */
	uint16_t addr;           /* word address within the array */
	uint16_t data;
	uint8_t cmd;
	eeprom_cmd_t active, queued;   /* the command executing and the one buffered behind it */
	bool has_active, has_queued;
} eeprom_t;

typedef struct atd {
	bool busy;
	uint64_t start;
	int conv_index;          /* conversion within the sequence */
} atd_t;

#define CAN_RXFIFO 5
typedef struct can_frame {
	uint8_t idr[4];
	uint8_t dsr[8];
	uint8_t dlr;
	uint8_t tbpr;
} can_frame_t;

typedef struct mscan {
	can_frame_t tx[3];       /* transmit buffers TX0-TX2 */
	can_frame_t rx[CAN_RXFIFO];
	int rx_head, rx_count;   /* foreground = rx[rx_head] when RXF */
	int tx_busy;             /* buffer on the bus, -1 none */
	bool initak, slpak;
	uint64_t synch_due;      /* tick when SYNCH is reached after leaving init mode, 0 if none */
} mscan_t;

struct mcu {
	cpu12_t cpu;
	uint8_t flash[FLASH_PAGES][PAGE_SIZE];
	uint8_t ram[RAM_SIZE];
	uint8_t eeprom[EEPROM_SIZE];
	uint8_t regs[REG_SIZE];
	reg_rd_t rd[REG_SIZE];
	reg_wr_t wr[REG_SIZE];
	uint8_t reg_seen[REG_SIZE]; /* bit 0 read, bit 1 written: accesses to unmodelled registers */
	bool reg_plain[REG_SIZE];   /* plain storage by design */
	uint8_t written_once;       /* INITRM/INITRG/INITEE/MISC write-once bits */

	/* time */
	uint64_t now;               /* ticks */
	uint32_t osc_hz;
	uint32_t bus_ticks;         /* ticks per bus cycle */
	uint64_t ev_when[EV_COUNT]; /* UINT64_MAX: not scheduled */
	uint64_t next_event;

	/* interrupts: bit (vector - 0xFF80) / 2 */
	uint64_t irq_pending;
	bool xirq_pin;              /* asserted */

	ect_t ect;
	spi_t spi0;
	eeprom_t ee;
	atd_t atd0;
	mscan_t can0;
	uint8_t pins_reported[PORT_COUNT][2];

	board_if_t board;

	/* differential testing (tools/difftest.c) */
	bool irq_block;             /* take no I-maskable interrupt and no XIRQ */
	void (*write_hook)(mcu_t *m, uint16_t addr, uint8_t v);   /* every CPU write; NULL if none */
	void *hook_ctx;

	/* debugging */
	uint8_t *breakpoints;       /* 64K bitmap entries, NULL if none */
	bool stop;                  /* stop the run loop after the current instruction */
	const char *stop_reason;
	uint16_t trace[4096];       /* recent instruction addresses */
	unsigned trace_pos;
	FILE *log;                  /* diagnostics (unmodelled registers, ...) */
};

void mcu_init(mcu_t *m, uint32_t osc_hz, FILE *log);
void mcu_reset(mcu_t *m);
/* Run until the given tick, a breakpoint, a CPU halt or m->stop. Returns m->now. */
uint64_t mcu_run(mcu_t *m, uint64_t until);

/* Bus access as the CPU sees it (side effects included); peek has none. */
uint8_t mcu_read(mcu_t *m, uint16_t addr);
void mcu_write(mcu_t *m, uint16_t addr, uint8_t v);
uint8_t mcu_peek(mcu_t *m, uint16_t addr);

/* Peripheral plumbing */
void mcu_hook(mcu_t *m, uint16_t off, int count, reg_rd_t rd, reg_wr_t wr);
void mcu_plain(mcu_t *m, uint16_t off, int count);
void mcu_schedule(mcu_t *m, int ev, uint64_t when);
void mcu_irq(mcu_t *m, uint16_t vector, bool level);
uint16_t mcu_reg16(mcu_t *m, uint16_t off);
static inline uint64_t mcu_bus_cycles(const mcu_t *m, uint64_t n) { return n * m->bus_ticks; }
double mcu_seconds(const mcu_t *m, uint64_t ticks);
uint64_t mcu_ticks(const mcu_t *m, double seconds);
void mcu_ports_update(mcu_t *m);
uint8_t mcu_port_pins(mcu_t *m, int port);

/* Register offsets used across modules */
enum {
	R_PORTA = 0x00, R_PORTB = 0x01, R_DDRA = 0x02, R_DDRB = 0x03, R_PORTE = 0x08, R_DDRE = 0x09,
	R_PEAR = 0x0A, R_MODE = 0x0B, R_PUCR = 0x0C, R_INITRM = 0x10, R_INITRG = 0x11, R_INITEE = 0x12,
	R_MISC = 0x13, R_INTCR = 0x1E, R_HPRIO = 0x1F, R_PPAGE = 0x30, R_PORTK = 0x32, R_DDRK = 0x33,
	R_SYNR = 0x34, R_REFDV = 0x35, R_CRGFLG = 0x37, R_CRGINT = 0x38, R_CLKSEL = 0x39, R_PLLCTL = 0x3A,
	R_RTICTL = 0x3B, R_COPCTL = 0x3C, R_ARMCOP = 0x3F,
	R_PTT = 0x240, R_PTIT = 0x241, R_DDRT = 0x242, R_PERT = 0x244, R_PPST = 0x245,
	R_PTS = 0x248, R_PTM = 0x250, R_PTIM = 0x251, R_DDRM = 0x252, R_PERM = 0x254, R_PPSM = 0x255,
	R_MODRR = 0x257, R_PTP = 0x258, R_PTH = 0x260, R_PTJ = 0x268,
};

/* Peripheral modules */
void ect_init(mcu_t *m);
void ect_reset(mcu_t *m);
void ect_event(mcu_t *m);
void ect_mdc_event(mcu_t *m);
void ect_pins_changed(mcu_t *m, uint8_t pt_levels);
uint8_t ect_pt_override(mcu_t *m, uint8_t *mask);
void spi_init(mcu_t *m);
void spi_reset(mcu_t *m);
void spi_event(mcu_t *m);
void eeprom_init(mcu_t *m);
void eeprom_reset(mcu_t *m);
void eeprom_event(mcu_t *m);
bool eeprom_array_write(mcu_t *m, uint16_t index, uint8_t v);
void atd_init(mcu_t *m);
void atd_reset(mcu_t *m);
void atd_event(mcu_t *m);
void mscan_init(mcu_t *m);
void mscan_reset(mcu_t *m);
void mscan_event(mcu_t *m);
/* A frame from the bus; false if MSCAN0 did not accept it (off, filtered, FIFO full). */
bool mscan_receive(mcu_t *m, const uint8_t idr[4], uint8_t dlc, const uint8_t *data);

#endif
