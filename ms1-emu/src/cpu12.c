/* HCS12 CPU core: see cpu12.h. Section and table references are to the S12CPUV2 Reference
 * Manual, Rev. 4.0. */
#include "cpu12.h"

#include "cpu12_cycles.inc"

/* ---- bus ---- */

static inline uint8_t rd8(cpu12_t *c, uint16_t a) { return c->bus.read(c->bus.ctx, a); }
static inline void wr8(cpu12_t *c, uint16_t a, uint8_t v) { c->bus.write(c->bus.ctx, a, v); }

static inline uint16_t rd16(cpu12_t *c, uint16_t a)
{
	uint8_t hi = rd8(c, a);
	return (uint16_t)(hi << 8 | rd8(c, (uint16_t)(a + 1)));
}

static inline void wr16(cpu12_t *c, uint16_t a, uint16_t v)
{
	wr8(c, a, (uint8_t)(v >> 8));
	wr8(c, (uint16_t)(a + 1), (uint8_t)v);
}

static inline uint8_t fetch8(cpu12_t *c) { return rd8(c, c->pc++); }

static inline uint16_t fetch16(cpu12_t *c)
{
	uint16_t v = rd16(c, c->pc);
	c->pc += 2;
	return v;
}

static inline void push8(cpu12_t *c, uint8_t v) { wr8(c, --c->sp, v); }
static inline void push16(cpu12_t *c, uint16_t v) { c->sp -= 2; wr16(c, c->sp, v); }
static inline uint8_t pull8(cpu12_t *c) { return rd8(c, c->sp++); }

static inline uint16_t pull16(cpu12_t *c)
{
	uint16_t v = rd16(c, c->sp);
	c->sp += 2;
	return v;
}

static inline void set_d(cpu12_t *c, uint16_t d)
{
	c->a = (uint8_t)(d >> 8);
	c->b = (uint8_t)d;
}

/* ---- condition codes ---- */

static inline void flag(cpu12_t *c, uint8_t f, bool on)
{
	c->ccr = on ? (uint8_t)(c->ccr | f) : (uint8_t)(c->ccr & ~f);
}

static inline void nz8(cpu12_t *c, uint8_t r)
{
	flag(c, CCR_N, r & 0x80);
	flag(c, CCR_Z, r == 0);
}

static inline void nz16(cpu12_t *c, uint16_t r)
{
	flag(c, CCR_N, r & 0x8000);
	flag(c, CCR_Z, r == 0);
}

/* Load/store/logic: N Z from the value, V cleared. */
static inline void nzv8(cpu12_t *c, uint8_t r) { nz8(c, r); flag(c, CCR_V, false); }
static inline void nzv16(cpu12_t *c, uint16_t r) { nz16(c, r); flag(c, CCR_V, false); }

/* The X mask bit can be cleared by software but set only by reset or XIRQ recognition. */
static inline void set_ccr(cpu12_t *c, uint8_t v)
{
	if (!(c->ccr & CCR_X))
		v &= (uint8_t)~CCR_X;
	c->ccr = v;
}

static uint8_t add8(cpu12_t *c, uint8_t a, uint8_t m, int carry)
{
	uint8_t r = (uint8_t)(a + m + carry);
	unsigned cy = (unsigned)((a & m) | (m & ~r) | (~r & a));
	flag(c, CCR_H, cy & 0x08);
	flag(c, CCR_C, cy & 0x80);
	flag(c, CCR_V, ((a & m & ~r) | (~a & ~m & r)) & 0x80);
	nz8(c, r);
	return r;
}

static uint8_t sub8(cpu12_t *c, uint8_t a, uint8_t m, int borrow)
{
	uint8_t r = (uint8_t)(a - m - borrow);
	flag(c, CCR_C, ((~a & m) | (m & r) | (r & ~a)) & 0x80);
	flag(c, CCR_V, ((a & ~m & ~r) | (~a & m & r)) & 0x80);
	nz8(c, r);
	return r;
}

static uint16_t add16(cpu12_t *c, uint16_t a, uint16_t m)
{
	uint16_t r = (uint16_t)(a + m);
	flag(c, CCR_C, ((a & m) | (m & ~r) | (~r & a)) & 0x8000);
	flag(c, CCR_V, ((a & m & ~r) | (~a & ~m & r)) & 0x8000);
	nz16(c, r);
	return r;
}

static uint16_t sub16(cpu12_t *c, uint16_t a, uint16_t m)
{
	uint16_t r = (uint16_t)(a - m);
	flag(c, CCR_C, ((~a & m) | (m & r) | (r & ~a)) & 0x8000);
	flag(c, CCR_V, ((a & ~m & ~r) | (~a & m & r)) & 0x8000);
	nz16(c, r);
	return r;
}

/* NEG COM INC DEC LSR ROL ROR ASR ASL CLR, in opcode order (0x40/0x50/0x60/0x70 + op). */
static uint8_t unary(cpu12_t *c, int op, uint8_t v)
{
	uint8_t r;
	bool cin = c->ccr & CCR_C;
	switch (op) {
	case 0: /* NEG */
		r = (uint8_t)-v;
		nz8(c, r);
		flag(c, CCR_V, r == 0x80);
		flag(c, CCR_C, r != 0);
		return r;
	case 1: /* COM */
		r = (uint8_t)~v;
		nzv8(c, r);
		flag(c, CCR_C, true);
		return r;
	case 2: /* INC */
		r = (uint8_t)(v + 1);
		nz8(c, r);
		flag(c, CCR_V, r == 0x80);
		return r;
	case 3: /* DEC */
		r = (uint8_t)(v - 1);
		nz8(c, r);
		flag(c, CCR_V, r == 0x7F);
		return r;
	case 4: /* LSR */
		r = v >> 1;
		flag(c, CCR_C, v & 1);
		break;
	case 5: /* ROL */
		r = (uint8_t)(v << 1 | cin);
		flag(c, CCR_C, v & 0x80);
		break;
	case 6: /* ROR */
		r = (uint8_t)(v >> 1 | cin << 7);
		flag(c, CCR_C, v & 1);
		break;
	case 7: /* ASR */
		r = (uint8_t)(v >> 1 | (v & 0x80));
		flag(c, CCR_C, v & 1);
		break;
	case 8: /* ASL */
		r = (uint8_t)(v << 1);
		flag(c, CCR_C, v & 0x80);
		break;
	default: /* CLR */
		c->ccr = (uint8_t)((c->ccr & ~(CCR_N | CCR_V | CCR_C)) | CCR_Z);
		return 0;
	}
	/* shifts and rotates: V = N xor C */
	nz8(c, r);
	flag(c, CCR_V, !!(r & 0x80) != !!(c->ccr & CCR_C));
	return r;
}

/* ---- indexed addressing (Table A-3) ---- */

enum { FORM_IDX, FORM_IDX1, FORM_IDX2, FORM_IND_D, FORM_IND_IDX2 };

typedef struct {
	uint8_t xb;
	uint8_t form;     /* cycle-table column */
	uint16_t offset;  /* constant offset (5-, 9- or 16-bit, sign-extended) */
} idx_t;

/* Fetch the postbyte and its extension bytes. */
static void idx_fetch(cpu12_t *c, idx_t *ix)
{
	uint8_t xb = ix->xb = fetch8(c);
	ix->form = FORM_IDX;
	ix->offset = 0;
	if (!(xb & 0x20)) {                         /* rr0nnnnn: 5-bit constant offset */
		ix->offset = (uint16_t)((xb & 0x10) ? (xb & 0x1F) - 0x20 : (xb & 0x1F));
	} else if ((xb & 0xE0) == 0xE0 && !(xb & 0x04)) {
		if (!(xb & 0x02)) {                     /* 111rr00s: 9-bit offset */
			ix->form = FORM_IDX1;
			ix->offset = (uint16_t)(fetch8(c) | ((xb & 1) ? 0xFF00 : 0));
		} else {                                /* 111rr010: 16-bit offset; 111rr011: [n16,r] */
			ix->form = (xb & 1) ? FORM_IND_IDX2 : FORM_IDX2;
			ix->offset = fetch16(c);
		}
	} else if ((xb & 0xE7) == 0xE7) {           /* 111rr111: [D,r] */
		ix->form = FORM_IND_D;
	}
}

static uint16_t *idx_reg(cpu12_t *c, int rr)
{
	return rr == 0 ? &c->x : rr == 1 ? &c->y : &c->sp;
}

/* Base register value: PC-relative forms use the address after the whole instruction, which
 * is c->pc once every byte of the instruction has been fetched (section 3.10.1, HCS12). */
static uint16_t idx_base(cpu12_t *c, int rr)
{
	return rr == 3 ? c->pc : *idx_reg(c, rr);
}

/* Address the postbyte selects, before any indirection; applies auto increment/decrement.
 * Call after the instruction's last byte has been fetched. */
static uint16_t idx_addr(cpu12_t *c, const idx_t *ix, bool *indirect)
{
	uint8_t xb = ix->xb;
	*indirect = false;
	if (!(xb & 0x20))
		return (uint16_t)(idx_base(c, xb >> 6 & 3) + ix->offset);
	if ((xb & 0xE0) != 0xE0) {                  /* rr1pnnnn: auto pre/post inc/dec */
		uint16_t *r = idx_reg(c, xb >> 6 & 3);
		int n = xb & 0x0F;
		uint16_t delta = (uint16_t)(n & 0x08 ? n - 16 : n + 1);
		if (xb & 0x10) {                        /* post */
			uint16_t ea = *r;
			*r += delta;
			return ea;
		}
		*r += delta;
		return *r;
	}
	int rr = xb >> 3 & 3;
	if (!(xb & 0x04)) {
		*indirect = ix->form == FORM_IND_IDX2;
		return (uint16_t)(idx_base(c, rr) + ix->offset);
	}
	switch (xb & 3) {
	case 0: return (uint16_t)(idx_base(c, rr) + c->a);   /* accumulator offsets are unsigned */
	case 1: return (uint16_t)(idx_base(c, rr) + c->b);
	case 2: return (uint16_t)(idx_base(c, rr) + cpu12_d(c));
	default:
		*indirect = true;
		return (uint16_t)(idx_base(c, rr) + cpu12_d(c));
	}
}

static uint16_t idx_ea(cpu12_t *c, const idx_t *ix)
{
	bool indirect;
	uint16_t a = idx_addr(c, ix, &indirect);
	return indirect ? rd16(c, a) : a;
}

/* ---- helpers ---- */

static int halt(cpu12_t *c, const char *why)
{
	c->state = CPU12_HALT;
	c->halt_reason = why;
	c->pc = c->insn_pc;
	return 0;
}

static void stack_context(cpu12_t *c)
{
	push16(c, c->pc);
	push16(c, c->y);
	push16(c, c->x);
	push16(c, (uint16_t)(c->b << 8 | c->a));  /* B:A => M(SP):M(SP+1) */
	push8(c, c->ccr);
}

static bool branch_taken(cpu12_t *c, uint8_t op)
{
	uint8_t ccr = c->ccr;
	bool n = ccr & CCR_N, z = ccr & CCR_Z, v = ccr & CCR_V, cy = ccr & CCR_C;
	switch (op & 0x0F) {
	case 0x0: return true;
	case 0x1: return false;
	case 0x2: return !(cy || z);
	case 0x3: return cy || z;
	case 0x4: return !cy;
	case 0x5: return cy;
	case 0x6: return !z;
	case 0x7: return z;
	case 0x8: return !v;
	case 0x9: return v;
	case 0xA: return !n;
	case 0xB: return n;
	case 0xC: return n == v;
	case 0xD: return n != v;
	case 0xE: return !z && n == v;
	default:  return z || n != v;
	}
}

/* TFR/EXG register numbers (Tables A-5): 0 A, 1 B, 2 CCR, 3 TMP3 (source) / TMP2 (dest),
 * 4 D, 5 X, 6 Y, 7 SP. */
static uint16_t reg_get(cpu12_t *c, int r, bool src)
{
	switch (r) {
	case 0: return c->a;
	case 1: return c->b;
	case 2: return c->ccr;
	case 3: return src ? c->tmp3 : c->tmp2;
	case 4: return cpu12_d(c);
	case 5: return c->x;
	case 6: return c->y;
	default: return c->sp;
	}
}

static void reg_set(cpu12_t *c, int r, bool src, uint16_t v)
{
	switch (r) {
	case 0: c->a = (uint8_t)v; break;
	case 1: c->b = (uint8_t)v; break;
	case 2: set_ccr(c, (uint8_t)v); break;
	case 3: if (src) c->tmp3 = v; else c->tmp2 = v; break;
	case 4: set_d(c, v); break;
	case 5: c->x = v; break;
	case 6: c->y = v; break;
	default: c->sp = v; break;
	}
}

static inline bool is8(int r) { return r < 3; }

static void tfr(cpu12_t *c, int src, int dst)
{
	uint16_t v = reg_get(c, src, true);
	if (is8(src) && !is8(dst))
		v = (uint16_t)(int8_t)v;  /* sign extension (SEX) */
	reg_set(c, dst, false, v);
}

/* EXG follows the EXG glossary table: an 8-bit register exchanged with a 16-bit one. */
static void exg(cpu12_t *c, int r1, int r2)
{
	uint16_t v1 = reg_get(c, r1, true), v2 = reg_get(c, r2, false);
	if (is8(r1) == is8(r2)) {
		reg_set(c, r2, false, v1);
		reg_set(c, r1, true, v2);
	} else if (is8(r1)) {
		/* $00:r1 => r2, r2 low => r1 (D destination: $00:A => D, $00:B => D, CCR: B => CCR) */
		if (r2 == 4) {
			uint8_t b = c->b;
			set_d(c, (uint8_t)v1);
			if (r1 == 2)
				set_ccr(c, b);
		} else {
			reg_set(c, r2, false, (uint8_t)v1);
			reg_set(c, r1, true, (uint8_t)v2);
		}
	} else {
		/* r1 low => r2; r2 extended => r1 ($00:A, $FF:B, $FF:CCR) */
		uint16_t ext = (uint16_t)(r2 == 0 ? (uint8_t)v2 : 0xFF00 | (uint8_t)v2);
		if (r1 == 4) {
			uint8_t a = c->a, b = c->b;
			if (r2 == 0) {        /* B => A, A => B */
				c->a = b;
				c->b = a;
			} else if (r2 == 1) { /* B => B, $FF => A */
				c->a = 0xFF;
			} else {              /* B => CCR, $FF:CCR => D */
				uint8_t ccr = c->ccr;
				set_ccr(c, b);
				set_d(c, (uint16_t)(0xFF00 | ccr));
			}
		} else {
			reg_set(c, r2, false, (uint8_t)v1);
			reg_set(c, r1, true, ext);
		}
	}
}

/* ---- exceptions ---- */

static int exception(cpu12_t *c, uint16_t vector, bool xirq)
{
	int cycles;
	if (c->state == CPU12_WAIT || c->state == CPU12_STOP) {
		cycles = 5;                   /* context already stacked by WAI/STOP: VfPPP */
	} else if (c->rti_chain) {
		/* RTI with an interrupt pending: the frame RTI just unstacked is still in memory and
		 * holds exactly the restored context, so SP moves back over it (11 cycles in all) */
		c->sp -= 9;
		cycles = 3;
	} else {
		stack_context(c);
		cycles = 9;                   /* VSPSSPSsP */
	}
	c->rti_chain = false;
	c->state = CPU12_RUN;
	c->ccr |= CCR_I;
	if (xirq)
		c->ccr |= CCR_X;
	c->last_vector = vector;
	c->pc = rd16(c, vector);
	return cycles;
}

void cpu12_reset(cpu12_t *c, uint16_t vector)
{
	c->a = c->b = 0;
	c->x = c->y = c->sp = 0;
	c->tmp2 = c->tmp3 = 0;
	c->ccr = CCR_S | CCR_X | CCR_I;
	c->state = CPU12_RUN;
	c->halt_reason = 0;
	c->rti_chain = false;
	c->last_vector = vector;
	c->pc = rd16(c, vector);
	c->insn_pc = c->pc;
}

/* ---- execution ---- */

/* Operand address for the 0x80-0xFF and 0x40-0x7F groups: mode from the opcode's high bits. */
enum { M_IMM, M_DIR, M_IDX, M_EXT };

static uint16_t operand_addr(cpu12_t *c, int mode, int *form)
{
	idx_t ix;
	*form = FORM_IDX;
	switch (mode) {
	case M_DIR: return fetch8(c);
	case M_EXT: return fetch16(c);
	default:
		idx_fetch(c, &ix);
		*form = ix.form;
		return idx_ea(c, &ix);
	}
}

static uint16_t *index16(cpu12_t *c, int n)  /* 0 X, 1 Y, 2 SP */
{
	return n == 0 ? &c->x : n == 1 ? &c->y : &c->sp;
}

/* 0x80-0xFF: accumulator/memory operations. */
static int exec_alu(cpu12_t *c, uint8_t op)
{
	int mode = op >> 4 & 3, form = FORM_IDX;
	bool accb = op & 0x40;
	int low = op & 0x0F;
	uint8_t *acc = accb ? &c->b : &c->a;
	bool wide = low == 0x03 || low >= 0x0C;
	uint16_t m = 0;

	if (low == 0x07) {
		/* 87 CLRA, 97 TSTA, A7 NOP, B7 TFR/EXG, C7 CLRB, D7 TSTB, E7 TST idx, F7 TST ext */
		switch (op) {
		case 0x87: c->a = unary(c, 9, 0); break;
		case 0xC7: c->b = unary(c, 9, 0); break;
		case 0x97: nzv8(c, c->a); flag(c, CCR_C, false); break;
		case 0xD7: nzv8(c, c->b); flag(c, CCR_C, false); break;
		case 0xA7: break;
		case 0xB7: {
			uint8_t eb = fetch8(c);
			if (eb & 0x80)
				exg(c, eb >> 4 & 7, eb & 7);
			else
				tfr(c, eb >> 4 & 7, eb & 7);
			break;
		}
		default: { /* TST memory */
			uint16_t a = operand_addr(c, op == 0xE7 ? M_IDX : M_EXT, &form);
			nzv8(c, rd8(c, a));
			flag(c, CCR_C, false);
		}
		}
		return cpu12_cycles_p1[op][form];
	}

	uint16_t addr = 0;
	if (mode == M_IMM) {
		m = wide ? fetch16(c) : fetch8(c);
	} else {
		addr = operand_addr(c, mode, &form);
		m = wide ? rd16(c, addr) : rd8(c, addr);
	}

	if (wide) {
		uint16_t d = cpu12_d(c);
		switch (low) {
		case 0x03: set_d(c, accb ? add16(c, d, m) : sub16(c, d, m)); break; /* ADDD / SUBD */
		case 0x0C: if (accb) { set_d(c, m); nzv16(c, m); } else sub16(c, d, m); break; /* LDD / CPD */
		case 0x0D: if (accb) { c->y = m; nzv16(c, m); } else sub16(c, c->y, m); break; /* LDY / CPY */
		case 0x0E: if (accb) { c->x = m; nzv16(c, m); } else sub16(c, c->x, m); break; /* LDX / CPX */
		default:   if (accb) { c->sp = m; nzv16(c, m); } else sub16(c, c->sp, m); break; /* LDS / CPS */
		}
		return cpu12_cycles_p1[op][form];
	}

	uint8_t v = (uint8_t)m;
	switch (low) {
	case 0x0: *acc = sub8(c, *acc, v, 0); break;                     /* SUB */
	case 0x1: sub8(c, *acc, v, 0); break;                            /* CMP */
	case 0x2: *acc = sub8(c, *acc, v, c->ccr & CCR_C); break;        /* SBC */
	case 0x4: *acc &= v; nzv8(c, *acc); break;                       /* AND */
	case 0x5: nzv8(c, *acc & v); break;                              /* BIT */
	case 0x6: *acc = v; nzv8(c, v); break;                           /* LDA */
	case 0x8: *acc ^= v; nzv8(c, *acc); break;                       /* EOR */
	case 0x9: *acc = add8(c, *acc, v, c->ccr & CCR_C); break;        /* ADC */
	case 0xA: *acc |= v; nzv8(c, *acc); break;                       /* ORA */
	default:  *acc = add8(c, *acc, v, 0); break;                     /* ADD */
	}
	return cpu12_cycles_p1[op][form];
}

/* 0x40-0x7F */
static int exec_unary_store(cpu12_t *c, uint8_t op)
{
	int form = FORM_IDX;
	int low = op & 0x0F;
	if (low <= 0x09) {
		if (op < 0x60) {
			uint8_t *acc = op < 0x50 ? &c->a : &c->b;
			if (low == 0x09) {               /* 49 LSRD, 59 ASLD */
				uint16_t d = cpu12_d(c), r;
				if (op == 0x49) {
					r = d >> 1;
					flag(c, CCR_C, d & 1);
				} else {
					r = (uint16_t)(d << 1);
					flag(c, CCR_C, d & 0x8000);
				}
				set_d(c, r);
				nz16(c, r);
				flag(c, CCR_V, !!(r & 0x8000) != !!(c->ccr & CCR_C));
			} else {
				*acc = unary(c, low, *acc);
			}
			return cpu12_cycles_p1[op][form];
		}
		uint16_t a = operand_addr(c, op < 0x70 ? M_IDX : M_EXT, &form);
		if (low == 0x09)
			wr8(c, a, unary(c, 9, 0));       /* CLR writes without reading */
		else
			wr8(c, a, unary(c, low, rd8(c, a)));
		return cpu12_cycles_p1[op][form];
	}
	/* stores: 5A-5F dir, 6A-6F idx, 7A-7F ext: STAA STAB STD STY STX STS */
	int mode = op < 0x60 ? M_DIR : op < 0x70 ? M_IDX : M_EXT;
	uint16_t a = operand_addr(c, mode, &form);
	switch (low) {
	case 0xA: wr8(c, a, c->a); nzv8(c, c->a); break;
	case 0xB: wr8(c, a, c->b); nzv8(c, c->b); break;
	case 0xC: wr16(c, a, cpu12_d(c)); nzv16(c, cpu12_d(c)); break;
	case 0xD: wr16(c, a, c->y); nzv16(c, c->y); break;
	case 0xE: wr16(c, a, c->x); nzv16(c, c->x); break;
	default:  wr16(c, a, c->sp); nzv16(c, c->sp); break;
	}
	return cpu12_cycles_p1[op][form];
}

/* BSET/BCLR/BRSET/BRCLR (0C-0F idx, 1C-1F ext, 4C-4F dir) */
static int exec_bit(cpu12_t *c, uint8_t op)
{
	int form = FORM_IDX;
	idx_t ix;
	uint16_t a = 0;
	int kind = op & 3;           /* 0 BSET, 1 BCLR, 2 BRSET, 3 BRCLR */
	int mode = op < 0x10 ? M_IDX : op < 0x20 ? M_EXT : M_DIR;
	if (mode == M_IDX)
		idx_fetch(c, &ix);
	else
		a = mode == M_DIR ? fetch8(c) : fetch16(c);
	uint8_t mask = fetch8(c);
	int8_t rel = kind >= 2 ? (int8_t)fetch8(c) : 0;
	if (mode == M_IDX) {
		form = ix.form;
		a = idx_ea(c, &ix);
	}
	uint8_t v = rd8(c, a);
	switch (kind) {
	case 0: v |= mask; wr8(c, a, v); nzv8(c, v); break;
	case 1: v &= (uint8_t)~mask; wr8(c, a, v); nzv8(c, v); break;
	case 2: if ((~v & mask) == 0) c->pc += rel; break;
	default: if ((v & mask) == 0) c->pc += rel; break;
	}
	return cpu12_cycles_p1[op][form];
}

/* 04 lb rr: DBEQ DBNE TBEQ TBNE IBEQ IBNE (Table A-6) */
static int exec_loop(cpu12_t *c)
{
	uint8_t lb = fetch8(c);
	uint8_t rr = fetch8(c);
	int kind = lb >> 5, reg = lb & 7;
	if (kind > 5 || reg == 2 || reg == 3)
		return halt(c, "illegal loop primitive postbyte");
	int16_t rel = (int16_t)((lb & 0x10) ? 0xFF00 | rr : rr);
	uint16_t v = reg_get(c, reg, true);
	if (kind < 2)
		v--;
	else if (kind >= 4)
		v++;
	if (is8(reg))
		v &= 0xFF;
	if (kind < 2 || kind >= 4)
		reg_set(c, reg, true, v);
	bool zero = v == 0;
	if ((kind & 1) ? !zero : zero)
		c->pc += (uint16_t)rel;
	return cpu12_cycles_p1[0x04][0];
}

static int exec_call(cpu12_t *c, uint8_t op)
{
	int form = FORM_IDX;
	uint16_t target;
	uint8_t page;
	if (op == 0x4A) {
		target = fetch16(c);
		page = fetch8(c);
	} else {
		idx_t ix;
		bool indirect;
		idx_fetch(c, &ix);
		form = ix.form;
		if (form == FORM_IND_D || form == FORM_IND_IDX2) {
			uint16_t p = idx_addr(c, &ix, &indirect);  /* pointer: address, then page */
			target = rd16(c, p);
			page = rd8(c, (uint16_t)(p + 2));
		} else {
			page = fetch8(c);
			target = idx_ea(c, &ix);
		}
	}
	push16(c, c->pc);
	push8(c, c->bus.get_ppage(c->bus.ctx));
	c->bus.set_ppage(c->bus.ctx, page);
	c->pc = target;
	return cpu12_cycles_p1[op][form];
}

static int exec_page2(cpu12_t *c)
{
	uint8_t op = fetch8(c);
	int form = FORM_IDX;
	idx_t ix, iy;

	if (op >= 0x20 && op <= 0x2F) {             /* LBcc */
		int16_t rel = (int16_t)fetch16(c);
		if (branch_taken(c, op)) {
			c->pc += (uint16_t)rel;
			return cpu12_cycles_p2[op][0];
		}
		return cpu12_not_taken_p2[op] ? cpu12_not_taken_p2[op] : cpu12_cycles_p2[op][0];
	}
	switch (op) {
	/* MOVW / MOVB (section 3.10.1): indexed operands are IDX forms only */
	case 0x00: case 0x08: {                     /* #imm -> idx: xb then the immediate */
		idx_fetch(c, &ix);
		uint16_t v = op ? fetch8(c) : fetch16(c);
		uint16_t a = idx_ea(c, &ix);
		if (op) wr8(c, a, (uint8_t)v); else wr16(c, a, v);
		break;
	}
	case 0x01: case 0x09: {                     /* ext -> idx: xb then the source address */
		idx_fetch(c, &ix);
		uint16_t src = fetch16(c);
		uint16_t v = op == 0x09 ? rd8(c, src) : rd16(c, src);
		uint16_t a = idx_ea(c, &ix);
		if (op == 0x09) wr8(c, a, (uint8_t)v); else wr16(c, a, v);
		break;
	}
	case 0x02: case 0x0A: {                     /* idx -> idx */
		idx_fetch(c, &ix);
		idx_fetch(c, &iy);
		uint16_t s = idx_ea(c, &ix);
		uint16_t v = op == 0x0A ? rd8(c, s) : rd16(c, s);
		uint16_t a = idx_ea(c, &iy);
		if (op == 0x0A) wr8(c, a, (uint8_t)v); else wr16(c, a, v);
		break;
	}
	case 0x03: case 0x0B: {                     /* #imm -> ext */
		uint16_t v = op == 0x0B ? fetch8(c) : fetch16(c);
		uint16_t a = fetch16(c);
		if (op == 0x0B) wr8(c, a, (uint8_t)v); else wr16(c, a, v);
		break;
	}
	case 0x04: case 0x0C: {                     /* ext -> ext */
		uint16_t s = fetch16(c), a = fetch16(c);
		if (op == 0x0C) wr8(c, a, rd8(c, s)); else wr16(c, a, rd16(c, s));
		break;
	}
	case 0x05: case 0x0D: {                     /* idx -> ext: xb then the destination address */
		idx_fetch(c, &ix);
		uint16_t a = fetch16(c);
		uint16_t s = idx_ea(c, &ix);
		if (op == 0x0D) wr8(c, a, rd8(c, s)); else wr16(c, a, rd16(c, s));
		break;
	}
	case 0x06: c->a = add8(c, c->a, c->b, 0); break;               /* ABA */
	case 0x07: {                                                   /* DAA */
		uint8_t a = c->a, hi = a >> 4, lo = a & 0x0F, corr = 0;
		bool cy = c->ccr & CCR_C, h = c->ccr & CCR_H, cout = false;
		if (!cy) {
			if (!h && lo <= 9 && hi <= 9) corr = 0x00;
			else if (!h && lo >= 0xA && hi <= 8) corr = 0x06;
			else if (h && lo <= 3 && hi <= 9) corr = 0x06;
			else if (!h && lo <= 9 && hi >= 0xA) { corr = 0x60; cout = true; }
			else if (!h && lo >= 0xA && hi >= 9) { corr = 0x66; cout = true; }
			else if (h && lo <= 3 && hi >= 0xA) { corr = 0x66; cout = true; }
		} else {
			cout = true;
			if (!h && lo <= 9 && hi <= 2) corr = 0x60;
			else if (!h && lo >= 0xA && hi <= 2) corr = 0x66;
			else if (h && lo <= 3 && hi <= 3) corr = 0x66;
		}
		c->a = (uint8_t)(a + corr);
		nz8(c, c->a);
		flag(c, CCR_C, cout);
		break;
	}
	case 0x0E: c->b = c->a; nzv8(c, c->b); break;                  /* TAB */
	case 0x0F: c->a = c->b; nzv8(c, c->a); break;                  /* TBA */
	case 0x10: {                                                   /* IDIV */
		uint16_t d = cpu12_d(c), x = c->x;
		flag(c, CCR_V, false);
		if (x == 0) {
			c->x = 0xFFFF;
			flag(c, CCR_C, true);
			flag(c, CCR_Z, false);
		} else {
			c->x = d / x;
			set_d(c, d % x);
			flag(c, CCR_C, false);
			flag(c, CCR_Z, c->x == 0);
		}
		break;
	}
	case 0x11: {                                                   /* FDIV */
		uint16_t d = cpu12_d(c), x = c->x;
		if (x == 0 || x <= d) {
			c->x = 0xFFFF;
			flag(c, CCR_V, x <= d);
			flag(c, CCR_C, x == 0);
			flag(c, CCR_Z, false);
		} else {
			uint32_t n = (uint32_t)d << 16;
			c->x = (uint16_t)(n / x);
			set_d(c, (uint16_t)(n % x));
			flag(c, CCR_V, false);
			flag(c, CCR_C, false);
			flag(c, CCR_Z, c->x == 0);
		}
		break;
	}
	case 0x13: {                                                   /* EMULS */
		int32_t r = (int32_t)(int16_t)cpu12_d(c) * (int16_t)c->y;
		uint32_t u = (uint32_t)r;
		c->y = (uint16_t)(u >> 16);
		set_d(c, (uint16_t)u);
		flag(c, CCR_N, u & 0x80000000u);
		flag(c, CCR_Z, u == 0);
		flag(c, CCR_C, u & 0x8000);
		break;
	}
	case 0x14: {                                                   /* EDIVS */
		int32_t n = (int32_t)((uint32_t)c->y << 16 | cpu12_d(c));
		int16_t x = (int16_t)c->x;
		if (x == 0) {
			flag(c, CCR_C, true);
			break;
		}
		flag(c, CCR_C, false);
		int64_t q = (int64_t)n / x, r = (int64_t)n % x;
		if (q > 0x7FFF || q < -0x8000) {
			flag(c, CCR_V, true);
			break;
		}
		c->y = (uint16_t)q;
		set_d(c, (uint16_t)r);
		flag(c, CCR_V, false);
		nz16(c, c->y);
		break;
	}
	case 0x15: {                                                   /* IDIVS */
		int16_t d = (int16_t)cpu12_d(c), x = (int16_t)c->x;
		if (x == 0) {
			flag(c, CCR_C, true);
			break;
		}
		flag(c, CCR_C, false);
		int32_t q = d / x, r = d % x;
		flag(c, CCR_V, q > 0x7FFF);
		c->x = (uint16_t)q;
		set_d(c, (uint16_t)r);
		nz16(c, c->x);
		break;
	}
	case 0x16: c->a = sub8(c, c->a, c->b, 0); break;               /* SBA */
	case 0x17: sub8(c, c->a, c->b, 0); break;                      /* CBA */
	case 0x18: case 0x19: case 0x1C: case 0x1D: {                  /* MAXA MINA MAXM MINM */
		idx_fetch(c, &ix);
		form = ix.form;
		uint16_t a = idx_ea(c, &ix);
		uint8_t m = rd8(c, a);
		sub8(c, c->a, m, 0);
		bool a_lower = c->ccr & CCR_C;                 /* A < M, unsigned */
		bool a_higher = !a_lower && !(c->ccr & CCR_Z);
		if (op == 0x18 && a_lower) c->a = m;
		else if (op == 0x19 && a_higher) c->a = m;
		else if (op == 0x1C && a_higher) wr8(c, a, c->a);  /* M = max */
		else if (op == 0x1D && a_lower) wr8(c, a, c->a);   /* M = min */
		break;
	}
	case 0x1A: case 0x1B: case 0x1E: case 0x1F: {                  /* EMAXD EMIND EMAXM EMINM */
		idx_fetch(c, &ix);
		form = ix.form;
		uint16_t a = idx_ea(c, &ix);
		uint16_t m = rd16(c, a), d = cpu12_d(c);
		sub16(c, d, m);
		bool d_lower = c->ccr & CCR_C;
		bool d_higher = !d_lower && !(c->ccr & CCR_Z);
		if (op == 0x1A && d_lower) set_d(c, m);
		else if (op == 0x1B && d_higher) set_d(c, m);
		else if (op == 0x1E && d_higher) wr16(c, a, d);
		else if (op == 0x1F && d_lower) wr16(c, a, d);
		break;
	}
	case 0x3E:                                                     /* STOP */
		if (c->ccr & CCR_S)
			return 2;
		stack_context(c);
		c->state = CPU12_STOP;
		break;
	case 0x12: return halt(c, "EMACS not implemented");
	case 0x3A: case 0x3B: case 0x3C: return halt(c, "REV/REVW/WAV not implemented");
	case 0x3D: case 0x3F: return halt(c, "TBL/ETBL not implemented");
	default:                                                       /* TRAP */
		stack_context(c);
		c->ccr |= CCR_I;
		c->last_vector = VEC_TRAP;
		c->pc = rd16(c, VEC_TRAP);
		return 10;                                                 /* OVSPSSPSsP */
	}
	return cpu12_cycles_p2[op][form];
}

static int execute(cpu12_t *c)
{
	uint8_t op = fetch8(c);
	int form = FORM_IDX;
	idx_t ix;

	if (op >= 0x80)
		return exec_alu(c, op);
	if (op >= 0x40) {
		if (op == 0x4A || op == 0x4B)
			return exec_call(c, op);
		if (op >= 0x4C && op <= 0x4F)
			return exec_bit(c, op);
		return exec_unary_store(c, op);
	}
	if (op >= 0x20 && op <= 0x2F) {             /* Bcc rel8 */
		int8_t rel = (int8_t)fetch8(c);
		if (branch_taken(c, op)) {
			c->pc += (uint16_t)rel;
			return cpu12_cycles_p1[op][0];
		}
		return cpu12_not_taken_p1[op] ? cpu12_not_taken_p1[op] : cpu12_cycles_p1[op][0];
	}
	switch (op) {
	case 0x00: return halt(c, "BGND");
	case 0x01: return halt(c, "MEM not implemented");
	case 0x02: c->y++; flag(c, CCR_Z, c->y == 0); break;           /* INY */
	case 0x03: c->y--; flag(c, CCR_Z, c->y == 0); break;           /* DEY */
	case 0x04: return exec_loop(c);
	case 0x05:                                                     /* JMP idx */
		idx_fetch(c, &ix);
		form = ix.form;
		c->pc = idx_ea(c, &ix);
		break;
	case 0x06: c->pc = fetch16(c); break;                          /* JMP ext */
	case 0x07: {                                                   /* BSR */
		int8_t rel = (int8_t)fetch8(c);
		push16(c, c->pc);
		c->pc += (uint16_t)rel;
		break;
	}
	case 0x08: c->x++; flag(c, CCR_Z, c->x == 0); break;           /* INX */
	case 0x09: c->x--; flag(c, CCR_Z, c->x == 0); break;           /* DEX */
	case 0x0A: {                                                   /* RTC */
		uint8_t page = pull8(c);
		c->bus.set_ppage(c->bus.ctx, page);
		c->pc = pull16(c);
		break;
	}
	case 0x0B: {                                                   /* RTI */
		set_ccr(c, pull8(c));
		uint16_t ba = pull16(c);
		c->b = (uint8_t)(ba >> 8);
		c->a = (uint8_t)ba;
		c->x = pull16(c);
		c->y = pull16(c);
		c->pc = pull16(c);
		/* an interrupt pending now is entered without re-stacking (RTI access detail) */
		c->rti_chain = !(c->ccr & CCR_I) && c->bus.irq(c->bus.ctx);
		break;
	}
	case 0x0C: case 0x0D: case 0x0E: case 0x0F:
	case 0x1C: case 0x1D: case 0x1E: case 0x1F:
		return exec_bit(c, op);
	case 0x10: set_ccr(c, c->ccr & fetch8(c)); break;              /* ANDCC */
	case 0x11: {                                                   /* EDIV */
		uint32_t n = (uint32_t)c->y << 16 | cpu12_d(c);
		uint16_t x = c->x;
		if (x == 0) {
			flag(c, CCR_C, true);
			break;
		}
		flag(c, CCR_C, false);
		uint32_t q = n / x;
		if (q > 0xFFFF) {
			flag(c, CCR_V, true);
			break;
		}
		c->y = (uint16_t)q;
		set_d(c, (uint16_t)(n % x));
		flag(c, CCR_V, false);
		nz16(c, c->y);
		break;
	}
	case 0x12: {                                                   /* MUL */
		uint16_t r = (uint16_t)(c->a * c->b);
		set_d(c, r);
		flag(c, CCR_C, r & 0x80);
		break;
	}
	case 0x13: {                                                   /* EMUL */
		uint32_t r = (uint32_t)cpu12_d(c) * c->y;
		c->y = (uint16_t)(r >> 16);
		set_d(c, (uint16_t)r);
		flag(c, CCR_N, r & 0x80000000u);
		flag(c, CCR_Z, r == 0);
		flag(c, CCR_C, r & 0x8000);
		break;
	}
	case 0x14: set_ccr(c, c->ccr | fetch8(c)); break;              /* ORCC */
	case 0x15:                                                     /* JSR idx */
		idx_fetch(c, &ix);
		form = ix.form;
		{
			uint16_t t = idx_ea(c, &ix);
			push16(c, c->pc);
			c->pc = t;
		}
		break;
	case 0x16: case 0x17: {                                        /* JSR ext / dir */
		uint16_t t = op == 0x16 ? fetch16(c) : fetch8(c);
		push16(c, c->pc);
		c->pc = t;
		break;
	}
	case 0x18: return exec_page2(c);
	case 0x19: case 0x1A: case 0x1B: {                             /* LEAY LEAX LEAS */
		idx_fetch(c, &ix);
		form = ix.form;
		uint16_t ea = idx_ea(c, &ix);
		*index16(c, op == 0x1A ? 0 : op == 0x19 ? 1 : 2) = ea;
		break;
	}
	case 0x30: c->x = pull16(c); break;                            /* PULX */
	case 0x31: c->y = pull16(c); break;                            /* PULY */
	case 0x32: c->a = pull8(c); break;                             /* PULA */
	case 0x33: c->b = pull8(c); break;                             /* PULB */
	case 0x34: push16(c, c->x); break;                             /* PSHX */
	case 0x35: push16(c, c->y); break;                             /* PSHY */
	case 0x36: push8(c, c->a); break;                              /* PSHA */
	case 0x37: push8(c, c->b); break;                              /* PSHB */
	case 0x38: set_ccr(c, pull8(c)); break;                        /* PULC */
	case 0x39: push8(c, c->ccr); break;                            /* PSHC */
	case 0x3A: set_d(c, pull16(c)); break;                         /* PULD */
	case 0x3B: push16(c, cpu12_d(c)); break;                       /* PSHD */
	case 0x3C: return halt(c, "WAVR not implemented");
	case 0x3D: c->pc = pull16(c); break;                           /* RTS */
	case 0x3E:                                                     /* WAI */
		stack_context(c);
		c->state = CPU12_WAIT;
		break;
	case 0x3F:                                                     /* SWI */
		stack_context(c);
		c->ccr |= CCR_I;
		c->last_vector = VEC_SWI;
		c->pc = rd16(c, VEC_SWI);
		break;
	default:
		return halt(c, "unhandled opcode");
	}
	return cpu12_cycles_p1[op][form];
}

int cpu12_step(cpu12_t *c)
{
	int cycles;
	if (c->state == CPU12_HALT)
		return 0;
	if (!(c->ccr & CCR_X) && c->bus.xirq && c->bus.xirq(c->bus.ctx)) {
		cycles = exception(c, VEC_XIRQ, true);
	} else {
		uint16_t vector = (c->ccr & CCR_I) || c->irq_delay ? 0 : c->bus.irq(c->bus.ctx);
		c->irq_delay = false;
		if (vector) {
			cycles = exception(c, vector, false);
		} else if (c->state != CPU12_RUN) {
			return 0;
		} else {
			bool masked = c->ccr & CCR_I;
			c->rti_chain = false;
			c->insn_pc = c->pc;
			cycles = execute(c);
			/* clearing I takes effect one instruction late: the next instruction always runs
			 * (CLI glossary); RTI instead enters a pending interrupt at once */
			c->irq_delay = masked && !(c->ccr & CCR_I) && !c->rti_chain;
		}
	}
	c->cycles += (uint64_t)cycles;
	return cycles;
}
