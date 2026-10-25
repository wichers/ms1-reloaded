/* Flash image loading: S-records in the banked layout of tools/compose_image.py. */
#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexval(const char *s, int n, unsigned long *out)
{
	char buf[16];
	char *end;
	if (n >= (int)sizeof buf || (int)strlen(s) < n)
		return -1;
	memcpy(buf, s, (size_t)n);
	buf[n] = 0;
	*out = strtoul(buf, &end, 16);
	return *end ? -1 : 0;
}

int image_load_s19(const char *path, uint8_t flash[FLASH_PAGES][PAGE_SIZE], char *err, size_t errsize)
{
	FILE *f = fopen(path, "r");
	char line[600];
	int lineno = 0;
	if (!f) {
		snprintf(err, errsize, "%s: cannot open", path);
		return -1;
	}
	memset(flash, 0xFF, FLASH_PAGES * PAGE_SIZE);
	while (fgets(line, sizeof line, f)) {
		unsigned long count, addr, v;
		lineno++;
		if (line[0] != 'S' || (line[1] != '1' && line[1] != '2'))
			continue;
		int alen = line[1] == '1' ? 2 : 3;
		if (hexval(line + 2, 2, &count) || hexval(line + 4, alen * 2, &addr))
			goto bad;
		int ndata = (int)count - alen - 1;
		for (int i = 0; i < ndata; i++) {
			if (hexval(line + 4 + alen * 2 + i * 2, 2, &v))
				goto bad;
			unsigned long a = addr + (unsigned long)i;
			int page;
			unsigned long off;
			if (alen == 3) {
				page = (int)(a >> 16);
				off = (a & 0xFFFF) - 0x8000;
				if (page < FIRST_PAGE || page > FIRST_PAGE + FLASH_PAGES - 1 || off >= PAGE_SIZE)
					goto bad;
			} else if (a >= 0x4000 && a < 0x8000) {
				page = 0x3E;
				off = a - 0x4000;
			} else if (a >= 0xC000) {
				page = 0x3F;
				off = a - 0xC000;
			} else {
				goto bad;
			}
			flash[page - FIRST_PAGE][off] = (uint8_t)v;
		}
	}
	fclose(f);
	return 0;
bad:
	snprintf(err, errsize, "%s:%d: bad or out-of-range S-record", path, lineno);
	fclose(f);
	return -1;
}
