#ifndef IMAGE_H
#define IMAGE_H

#include <stddef.h>
#include <stdint.h>

/* MC9S12D64 flash: four 16 KB pages, PPAGE $3C-$3F; $3E is fixed at 0x4000 and $3F at 0xC000. */
#define FLASH_PAGES 4
#define FIRST_PAGE 0x3C
#define PAGE_SIZE 0x4000

/* Load an S-record file: S1 records for 0x4000-0x7FFF (page $3E) and 0xC000-0xFFFF ($3F), S2
 * records PPAGE:addr for windowed pages. Unwritten bytes are erased (0xFF). */
int image_load_s19(const char *path, uint8_t flash[FLASH_PAGES][PAGE_SIZE], char *err, size_t errsize);

/* Write the flash in the same form (records of erased bytes only are left out): 0, or -1 with
 * a message in err. */
int image_save_s19(const char *path, const uint8_t flash[FLASH_PAGES][PAGE_SIZE], char *err, size_t errsize);

#endif
