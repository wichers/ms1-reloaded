/* Decoder check: disassemble every instruction of a Ghidra listing (ms1-ghidra
 * out/listing.tsv: address, bytes, mnemonic, text) from the flash image and compare the
 * instruction bytes, length and mnemonic. Page $3D is the one in the PPAGE window.
 *   decode_check <image.s19> <listing.tsv>
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/cpu12.h"
#include "../src/image.h"

static uint8_t flash[FLASH_PAGES][PAGE_SIZE];

static uint8_t peek(void *ctx, uint16_t a)
{
	(void)ctx;
	if (a >= 0xC000) return flash[3][a - 0xC000];
	if (a >= 0x8000) return flash[1][a - 0x8000];
	if (a >= 0x4000) return flash[2][a - 0x4000];
	return 0xFF;
}

/* Mnemonic aliases: Ghidra prints the canonical name, the disassembler the alias or vice versa. */
static const char *const same[][2] = {
	{ "LSLA", "ASLA" }, { "LSLB", "ASLB" }, { "LSLD", "ASLD" }, { "LSL", "ASL" },
	{ "BHS", "BCC" }, { "BLO", "BCS" }, { "LBHS", "LBCC" }, { "LBLO", "LBCS" },
	{ "CLC", "ANDCC" }, { "CLI", "ANDCC" }, { "CLV", "ANDCC" }, { "SEC", "ORCC" }, { "SEI", "ORCC" },
	{ "SEV", "ORCC" }, { "TAP", "TFR" }, { "TPA", "TFR" }, { "TSX", "TFR" }, { "TSY", "TFR" },
	{ "TXS", "TFR" }, { "TYS", "TFR" }, { "SEX", "TFR" }, { "XGDX", "EXG" }, { "XGDY", "EXG" },
	{ "ABX", "LEAX" }, { "ABY", "LEAY" }, { "DES", "LEAS" }, { "INS", "LEAS" },
};

static const char *canon(const char *m)
{
	for (size_t i = 0; i < sizeof same / sizeof same[0]; i++)
		if (!strcmp(m, same[i][0]))
			return same[i][1];
	return m;
}

int main(int argc, char **argv)
{
	char err[256], line[512], text[128];
	if (argc != 3) {
		fprintf(stderr, "usage: %s <image.s19> <listing.tsv>\n", argv[0]);
		return 2;
	}
	if (image_load_s19(argv[1], flash, err, sizeof err)) {
		fprintf(stderr, "%s\n", err);
		return 2;
	}
	FILE *f = fopen(argv[2], "r");
	if (!f) {
		perror(argv[2]);
		return 2;
	}
	int n = 0, bad = 0;
	while (fgets(line, sizeof line, f)) {
		char *addr_s = strtok(line, "\t"), *bytes = strtok(0, "\t"), *mn = strtok(0, "\t\n");
		if (!addr_s || !bytes || !mn)
			continue;
		uint16_t addr = (uint16_t)strtoul(addr_s, 0, 16);
		int len = (int)strlen(bytes) / 2;
		n++;
		bool same_bytes = true;
		for (int i = 0; i < len; i++) {
			unsigned v;
			sscanf(bytes + 2 * i, "%2x", &v);
			same_bytes &= peek(0, (uint16_t)(addr + i)) == v;
		}
		int got = cpu12_disasm(peek, 0, addr, text, sizeof text);
		char m[16];
		sscanf(text, "%15s", m);
		for (char *p = mn; *p; p++)
			*p = (char)toupper((unsigned char)*p);
		const char *problem = !same_bytes ? "image bytes differ"
			: got != len ? "length"
			: strcmp(canon(m), canon(mn)) ? "mnemonic" : 0;
		if (problem) {
			bad++;
			if (bad <= 40)
				printf("%04X %-12s ghidra %-6s len %d | ours %-24s len %d  (%s)\n", addr, bytes, mn, len, text, got, problem);
		}
	}
	fclose(f);
	printf("%d instructions, %d mismatches\n", n, bad);
	return bad != 0;
}
