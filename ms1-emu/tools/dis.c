/* dis: disassemble a range of a flash image (S-records, as ms1emu loads them).
 *   dis IMAGE.s19 START END       hex CPU addresses, END exclusive; page $3D in the window */
#include <stdio.h>
#include <stdlib.h>

#include "../src/cpu12.h"
#include "../src/image.h"

static uint8_t flash[FLASH_PAGES][PAGE_SIZE];

static uint8_t peek(void *ctx, uint16_t a)
{
	(void)ctx;
	if (a >= 0xC000)
		return flash[3][a - 0xC000];
	if (a >= 0x8000)
		return flash[1][a - 0x8000];
	if (a >= 0x4000)
		return flash[2][a - 0x4000];
	return 0;
}

int main(int argc, char **argv)
{
	char err[256], text[64];
	if (argc != 4) {
		fprintf(stderr, "usage: dis IMAGE.s19 START END\n");
		return 2;
	}
	if (image_load_s19(argv[1], flash, err, sizeof err)) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	unsigned a = (unsigned)strtoul(argv[2], 0, 16), end = (unsigned)strtoul(argv[3], 0, 16);
	while (a < end) {
		int n = cpu12_disasm(peek, 0, (uint16_t)a, text, sizeof text);
		printf("%04X ", a);
		for (int i = 0; i < 6; i++)
			printf(i < n ? "%02X" : "  ", peek(0, (uint16_t)(a + i)));
		printf("  %s\n", text);
		a += (unsigned)n;
	}
	return 0;
}
