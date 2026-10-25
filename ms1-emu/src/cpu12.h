/* HCS12 (S12CPUV2) CPU core. Semantics and bus cycle counts follow the S12CPUV2 Reference
 * Manual, Rev. 4.0; the memory map, interrupt sources and PPAGE register belong to the MCU
 * model behind the bus callbacks. */
#ifndef CPU12_H
#define CPU12_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	CCR_C = 0x01, CCR_V = 0x02, CCR_Z = 0x04, CCR_N = 0x08,
	CCR_I = 0x10, CCR_H = 0x20, CCR_X = 0x40, CCR_S = 0x80,
};

enum {
	VEC_RESET = 0xFFFE, VEC_CLKMON = 0xFFFC, VEC_COP = 0xFFFA, VEC_TRAP = 0xFFF8,
	VEC_SWI = 0xFFF6, VEC_XIRQ = 0xFFF4,
};

enum cpu12_state {
	CPU12_RUN,
	CPU12_WAIT,  /* WAI executed: registers stacked, waiting for an interrupt */
	CPU12_STOP,  /* STOP executed with S clear */
	CPU12_HALT,  /* BGND, an illegal encoding or an instruction this core does not implement */
};

typedef struct cpu12_bus {
	void *ctx;
	uint8_t (*read)(void *ctx, uint16_t addr);
	void (*write)(void *ctx, uint16_t addr, uint8_t value);
	/* Vector address of the highest-priority pending I-maskable interrupt; 0 if none. */
	uint16_t (*irq)(void *ctx);
	/* XIRQ pin asserted (may be NULL). */
	bool (*xirq)(void *ctx);
	/* PPAGE, for CALL and RTC. */
	uint8_t (*get_ppage)(void *ctx);
	void (*set_ppage)(void *ctx, uint8_t page);
} cpu12_bus_t;

typedef struct cpu12 {
	uint8_t a, b, ccr;
	uint16_t x, y, sp, pc;
	uint16_t tmp2, tmp3;     /* hidden registers reachable through TFR/EXG */
	enum cpu12_state state;
	const char *halt_reason;
	uint16_t insn_pc;        /* address of the instruction executing (or last executed) */
	uint16_t last_vector;    /* vector of the last exception entry */
	bool rti_chain;          /* RTI found an interrupt pending: its entry skips re-stacking */
	bool irq_delay;          /* I was just cleared: the next instruction runs first */
	uint64_t cycles;         /* bus cycles since reset */
	cpu12_bus_t bus;
} cpu12_t;

/* Reset: CCR = S X I set, PC from the vector. */
void cpu12_reset(cpu12_t *c, uint16_t vector);

/* Take a pending exception or execute one instruction; returns the bus cycles used (0 when
 * waiting or stopped with nothing to do, or halted). */
int cpu12_step(cpu12_t *c);

/* Disassemble the instruction at pc into buf; returns its length in bytes. */
int cpu12_disasm(uint8_t (*peek)(void *ctx, uint16_t addr), void *ctx, uint16_t pc, char *buf, size_t size);

static inline uint16_t cpu12_d(const cpu12_t *c) { return (uint16_t)(c->a << 8 | c->b); }

#endif
