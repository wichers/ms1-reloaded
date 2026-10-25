/* CPU12 disassembler (Motorola syntax), following Tables A-2 to A-6 of the S12CPUV2
 * Reference Manual. */
#include <stdio.h>
#include <string.h>

#include "cpu12.h"

enum {
	INH, IM8, IM16, DIR, EXT, IDX, REL8, LOOP, BITD, BITE, BITX, BRD, BRE, BRX, XFER,
	CALLE, CALLX, PG2, REL16, MOVIMI, MOVEI, MOVII, MOVIME, MOVEE, MOVIE, TRAP, ILL,
};

typedef struct { const char *name; unsigned char mode; } op_t;

/* 0x80-0xFF follow the A/B x IMM/DIR/IDX/EXT pattern; listed by low nibble */
static const char *const alu_a[16] = { "SUBA", "CMPA", "SBCA", "SUBD", "ANDA", "BITA", "LDAA", 0,
	"EORA", "ADCA", "ORAA", "ADDA", "CPD", "CPY", "CPX", "CPS" };
static const char *const alu_b[16] = { "SUBB", "CMPB", "SBCB", "ADDD", "ANDB", "BITB", "LDAB", 0,
	"EORB", "ADCB", "ORAB", "ADDB", "LDD", "LDY", "LDX", "LDS" };
static const char *const unary_n[10] = { "NEG", "COM", "INC", "DEC", "LSR", "ROL", "ROR", "ASR", "ASL", "CLR" };
static const char *const stores[6] = { "STAA", "STAB", "STD", "STY", "STX", "STS" };
static const char *const branches[16] = { "BRA", "BRN", "BHI", "BLS", "BCC", "BCS", "BNE", "BEQ",
	"BVC", "BVS", "BPL", "BMI", "BGE", "BLT", "BGT", "BLE" };
static const char *const regs[8] = { "A", "B", "CCR", "TMP", "D", "X", "Y", "SP" };

static const op_t low_ops[0x40] = {
	[0x00] = { "BGND", INH }, [0x01] = { "MEM", INH }, [0x02] = { "INY", INH }, [0x03] = { "DEY", INH },
	[0x04] = { "", LOOP }, [0x05] = { "JMP", IDX }, [0x06] = { "JMP", EXT }, [0x07] = { "BSR", REL8 },
	[0x08] = { "INX", INH }, [0x09] = { "DEX", INH }, [0x0A] = { "RTC", INH }, [0x0B] = { "RTI", INH },
	[0x0C] = { "BSET", BITX }, [0x0D] = { "BCLR", BITX }, [0x0E] = { "BRSET", BRX }, [0x0F] = { "BRCLR", BRX },
	[0x10] = { "ANDCC", IM8 }, [0x11] = { "EDIV", INH }, [0x12] = { "MUL", INH }, [0x13] = { "EMUL", INH },
	[0x14] = { "ORCC", IM8 }, [0x15] = { "JSR", IDX }, [0x16] = { "JSR", EXT }, [0x17] = { "JSR", DIR },
	[0x18] = { "", PG2 }, [0x19] = { "LEAY", IDX }, [0x1A] = { "LEAX", IDX }, [0x1B] = { "LEAS", IDX },
	[0x1C] = { "BSET", BITE }, [0x1D] = { "BCLR", BITE }, [0x1E] = { "BRSET", BRE }, [0x1F] = { "BRCLR", BRE },
	[0x30] = { "PULX", INH }, [0x31] = { "PULY", INH }, [0x32] = { "PULA", INH }, [0x33] = { "PULB", INH },
	[0x34] = { "PSHX", INH }, [0x35] = { "PSHY", INH }, [0x36] = { "PSHA", INH }, [0x37] = { "PSHB", INH },
	[0x38] = { "PULC", INH }, [0x39] = { "PSHC", INH }, [0x3A] = { "PULD", INH }, [0x3B] = { "PSHD", INH },
	[0x3C] = { "WAVR", INH }, [0x3D] = { "RTS", INH }, [0x3E] = { "WAI", INH }, [0x3F] = { "SWI", INH },
};

static const op_t page2_ops[0x40] = {
	[0x00] = { "MOVW", MOVIMI }, [0x01] = { "MOVW", MOVEI }, [0x02] = { "MOVW", MOVII },
	[0x03] = { "MOVW", MOVIME }, [0x04] = { "MOVW", MOVEE }, [0x05] = { "MOVW", MOVIE },
	[0x06] = { "ABA", INH }, [0x07] = { "DAA", INH },
	[0x08] = { "MOVB", MOVIMI }, [0x09] = { "MOVB", MOVEI }, [0x0A] = { "MOVB", MOVII },
	[0x0B] = { "MOVB", MOVIME }, [0x0C] = { "MOVB", MOVEE }, [0x0D] = { "MOVB", MOVIE },
	[0x0E] = { "TAB", INH }, [0x0F] = { "TBA", INH }, [0x10] = { "IDIV", INH }, [0x11] = { "FDIV", INH },
	[0x12] = { "EMACS", EXT }, [0x13] = { "EMULS", INH }, [0x14] = { "EDIVS", INH }, [0x15] = { "IDIVS", INH },
	[0x16] = { "SBA", INH }, [0x17] = { "CBA", INH }, [0x18] = { "MAXA", IDX }, [0x19] = { "MINA", IDX },
	[0x1A] = { "EMAXD", IDX }, [0x1B] = { "EMIND", IDX }, [0x1C] = { "MAXM", IDX }, [0x1D] = { "MINM", IDX },
	[0x1E] = { "EMAXM", IDX }, [0x1F] = { "EMINM", IDX },
	[0x3A] = { "REV", INH }, [0x3B] = { "REVW", INH }, [0x3C] = { "WAV", INH }, [0x3D] = { "TBL", IDX },
	[0x3E] = { "STOP", INH }, [0x3F] = { "ETBL", IDX },
};

typedef struct {
	uint8_t (*peek)(void *ctx, uint16_t addr);
	void *ctx;
	uint16_t pc;
} rd_t;

static uint8_t next8(rd_t *r) { return r->peek(r->ctx, r->pc++); }

static uint16_t next16(rd_t *r)
{
	uint16_t hi = next8(r);
	return (uint16_t)(hi << 8 | next8(r));
}

static const char *const ixreg[4] = { "X", "Y", "SP", "PC" };

/* Indexed operand text; reads the postbyte's extension bytes. */
static void fmt_idx(rd_t *r, char *out, size_t n)
{
	uint8_t xb = next8(r);
	if (!(xb & 0x20)) {
		int off = (xb & 0x10) ? (xb & 0x1F) - 0x20 : (xb & 0x1F);
		snprintf(out, n, "%d,%s", off, ixreg[xb >> 6 & 3]);
	} else if ((xb & 0xE0) != 0xE0) {
		int v = xb & 0x0F;
		int d = (v & 0x08) ? 16 - v : v + 1;
		const char *rg = ixreg[xb >> 6 & 3];
		char sign = (v & 0x08) ? '-' : '+';
		if (xb & 0x10)
			snprintf(out, n, "%d,%s%c", d, rg, sign);
		else
			snprintf(out, n, "%d,%c%s", d, sign, rg);
	} else {
		const char *rg = ixreg[xb >> 3 & 3];
		if (!(xb & 0x04)) {
			if (!(xb & 0x02)) {
				int off = next8(r) - ((xb & 1) ? 0x100 : 0);
				snprintf(out, n, "%d,%s", off, rg);
			} else {
				uint16_t off = next16(r);
				snprintf(out, n, (xb & 1) ? "[$%04X,%s]" : "$%04X,%s", off, rg);
			}
		} else {
			static const char *const acc[4] = { "A", "B", "D", "[D" };
			snprintf(out, n, (xb & 3) == 3 ? "[D,%s]" : "%s,%s", (xb & 3) == 3 ? rg : acc[xb & 3], rg);
		}
	}
}

int cpu12_disasm(uint8_t (*peek)(void *ctx, uint16_t addr), void *ctx, uint16_t pc, char *buf, size_t size)
{
	rd_t r = { peek, ctx, pc };
	uint8_t op = next8(&r);
	const char *name = 0;
	int mode;
	char o1[48], o2[48];
	bool page2 = false;

	if (op >= 0x80) {
		int low = op & 0x0F, m = op >> 4 & 3;
		bool b = op & 0x40;
		if (low == 0x07) {
			static const char *const sp7[8] = { "CLRA", "TSTA", "NOP", "", "CLRB", "TSTB", "TST", "TST" };
			name = sp7[(op >> 4) - 8];
			mode = op == 0xB7 ? XFER : op == 0xE7 ? IDX : op == 0xF7 ? EXT : INH;
		} else {
			name = b ? alu_b[low] : alu_a[low];
			bool wide = low == 0x03 || low >= 0x0C;
			mode = m == 0 ? (wide ? IM16 : IM8) : m == 1 ? DIR : m == 2 ? IDX : EXT;
		}
	} else if (op >= 0x40) {
		int low = op & 0x0F;
		static char nm[8];
		if (op == 0x4A || op == 0x4B) {
			name = "CALL";
			mode = op == 0x4A ? CALLE : CALLX;
		} else if (op >= 0x4C && op <= 0x4F) {
			static const char *const bn[4] = { "BSET", "BCLR", "BRSET", "BRCLR" };
			name = bn[op & 3];
			mode = (op & 3) >= 2 ? BRD : BITD;
		} else if (low >= 0x0A) {
			name = stores[low - 0x0A];
			mode = op < 0x60 ? DIR : op < 0x70 ? IDX : EXT;
		} else if (op < 0x60) {
			if (low == 0x09) {
				name = op == 0x49 ? "LSRD" : "ASLD";
			} else {
				snprintf(nm, sizeof nm, "%s%c", unary_n[low], op < 0x50 ? 'A' : 'B');
				name = nm;
			}
			mode = INH;
		} else {
			name = unary_n[low];
			mode = op < 0x70 ? IDX : EXT;
		}
	} else if (op >= 0x20 && op <= 0x2F) {
		name = branches[op & 0x0F];
		mode = REL8;
	} else {
		name = low_ops[op].name;
		mode = low_ops[op].mode;
		if (mode == PG2) {
			page2 = true;
			op = next8(&r);
			if (op >= 0x20 && op <= 0x2F) {
				static char nm[8];
				snprintf(nm, sizeof nm, "L%s", branches[op & 0x0F]);
				name = nm;
				mode = REL16;
			} else if (op < 0x40 && page2_ops[op].name) {
				name = page2_ops[op].name;
				mode = page2_ops[op].mode;
			} else {
				name = "TRAP";
				mode = TRAP;
			}
		}
	}
	(void)page2;

	o1[0] = o2[0] = 0;
	switch (mode) {
	case INH: snprintf(buf, size, "%s", name); break;
	case IM8: snprintf(buf, size, "%s #$%02X", name, next8(&r)); break;
	case IM16: snprintf(buf, size, "%s #$%04X", name, next16(&r)); break;
	case DIR: snprintf(buf, size, "%s $%02X", name, next8(&r)); break;
	case EXT: snprintf(buf, size, "%s $%04X", name, next16(&r)); break;
	case IDX: fmt_idx(&r, o1, sizeof o1); snprintf(buf, size, "%s %s", name, o1); break;
	case REL8: { int8_t d = (int8_t)next8(&r); snprintf(buf, size, "%s $%04X", name, (uint16_t)(r.pc + d)); break; }
	case REL16: { int16_t d = (int16_t)next16(&r); snprintf(buf, size, "%s $%04X", name, (uint16_t)(r.pc + d)); break; }
	case LOOP: {
		static const char *const ln[8] = { "DBEQ", "DBNE", "TBEQ", "TBNE", "IBEQ", "IBNE", "?", "?" };
		uint8_t lb = next8(&r), rr = next8(&r);
		int d = (lb & 0x10) ? rr - 0x100 : rr;
		snprintf(buf, size, "%s %s,$%04X", ln[lb >> 5], regs[lb & 7], (uint16_t)(r.pc + d));
		break;
	}
	case BITD: case BRD: {
		uint8_t a = next8(&r), m = next8(&r);
		if (mode == BRD) { int8_t d = (int8_t)next8(&r); snprintf(buf, size, "%s $%02X,#$%02X,$%04X", name, a, m, (uint16_t)(r.pc + d)); }
		else snprintf(buf, size, "%s $%02X,#$%02X", name, a, m);
		break;
	}
	case BITE: case BRE: {
		uint16_t a = next16(&r);
		uint8_t m = next8(&r);
		if (mode == BRE) { int8_t d = (int8_t)next8(&r); snprintf(buf, size, "%s $%04X,#$%02X,$%04X", name, a, m, (uint16_t)(r.pc + d)); }
		else snprintf(buf, size, "%s $%04X,#$%02X", name, a, m);
		break;
	}
	case BITX: case BRX: {
		fmt_idx(&r, o1, sizeof o1);
		uint8_t m = next8(&r);
		if (mode == BRX) { int8_t d = (int8_t)next8(&r); snprintf(buf, size, "%s %s,#$%02X,$%04X", name, o1, m, (uint16_t)(r.pc + d)); }
		else snprintf(buf, size, "%s %s,#$%02X", name, o1, m);
		break;
	}
	case XFER: {
		uint8_t eb = next8(&r);
		int s = eb >> 4 & 7, d = eb & 7;
		if (eb & 0x80) {
			const char *alias = s == 4 && d == 5 ? "XGDX" : s == 4 && d == 6 ? "XGDY" : 0;
			if (alias) snprintf(buf, size, "%s", alias);
			else snprintf(buf, size, "EXG %s,%s", regs[s], regs[d]);
		} else {
			if (s < 3 && d >= 4) snprintf(buf, size, "SEX %s,%s", regs[s], regs[d]);
			else snprintf(buf, size, "TFR %s,%s", regs[s], regs[d]);
		}
		break;
	}
	case CALLE: { uint16_t a = next16(&r); snprintf(buf, size, "CALL $%04X,$%02X", a, next8(&r)); break; }
	case CALLX: {
		uint8_t xb = r.peek(r.ctx, r.pc);
		fmt_idx(&r, o1, sizeof o1);
		bool ind = (xb & 0xE7) == 0xE7 || (xb & 0xE7) == 0xE3;
		if (ind) snprintf(buf, size, "CALL %s", o1);
		else snprintf(buf, size, "CALL %s,$%02X", o1, next8(&r));
		break;
	}
	case MOVIMI: {
		fmt_idx(&r, o1, sizeof o1);
		if (name[3] == 'W') snprintf(buf, size, "%s #$%04X,%s", name, next16(&r), o1);
		else snprintf(buf, size, "%s #$%02X,%s", name, next8(&r), o1);
		break;
	}
	case MOVEI: fmt_idx(&r, o1, sizeof o1); snprintf(buf, size, "%s $%04X,%s", name, next16(&r), o1); break;
	case MOVII: fmt_idx(&r, o1, sizeof o1); fmt_idx(&r, o2, sizeof o2); snprintf(buf, size, "%s %s,%s", name, o1, o2); break;
	case MOVIME: {
		uint16_t v = name[3] == 'W' ? next16(&r) : next8(&r);
		snprintf(buf, size, name[3] == 'W' ? "%s #$%04X,$%04X" : "%s #$%02X,$%04X", name, v, next16(&r));
		break;
	}
	case MOVEE: { uint16_t s = next16(&r); snprintf(buf, size, "%s $%04X,$%04X", name, s, next16(&r)); break; }
	case MOVIE: fmt_idx(&r, o1, sizeof o1); snprintf(buf, size, "%s %s,$%04X", name, o1, next16(&r)); break;
	case TRAP: snprintf(buf, size, "TRAP #$%02X", op); break;
	default: snprintf(buf, size, "???"); break;
	}
	return (uint16_t)(r.pc - pc);
}
