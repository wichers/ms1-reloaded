/*==========================================================================
 * sys/ident.c - the identification area 0xF700-0xF73F and its strings
 *
 * The area the bootloader and the CAN identification read by address (docs/bootloader.md:
 * The application's side), with the values of the original release being built: the Makefile
 * compiles this file per release (MS1_RELEASE, MS1_ARTICLE) and links it on its own
 * (gcc/ident.ld), the strings below the area; tools/mkimage.py adds it to the release's image
 * and sets the CRC compensation word.
 *========================================================================*/

#include "../include/hcs12.h"

#ifndef MS1_ARTICLE
#define MS1_ARTICLE 60652
#endif

struct ms1_ident {
    u16 zero0;              /* 0xF700 */
    u16 version_word;       /* 0xF702: the application version word */
    u16 version;            /* 0xF704: the application version, major.minor (INFO AP:) */
    u16 build;              /* 0xF706: differs per release build */
    u32 build_time;         /* 0xF708: differs per release build, by the seconds the build
                             * dates differ */
    u16 zero1;              /* 0xF70C */
    u16 hw_code;            /* 0xF70E: the hardware code of the article (the bootloader's
                             * 0xFF1A) */
    u16 zero2[8];           /* 0xF710 */
    const u8 *product;      /* 0xF720: sent as CAN identification 3/3 */
    const u8 *build_date;   /* 0xF722 */
    const u8 *author;       /* 0xF724 */
    const u8 *copyright;    /* 0xF726 */
    u16 zero3[9];           /* 0xF728 */
    const u8 *article;      /* 0xF73A: INFO AN: */
    u16 version_check;      /* 0xF73C: the word the bootloader checks against 0xFF1A */
    u16 crc;                /* 0xF73E: the CRC compensation word, tools/mkimage.py */
};

#define T(s)    ((const u8 *)(s))

#if MS1_RELEASE == 0x4010
#define PRODUCT "Mobile Station MFx Motorola"
#if MS1_ARTICLE == 60651
#define BUILD 0x000E
#define BUILD_TIME 0x0ED03CC9
#define BUILD_DATE "Nov 16 2007 11:34:35"
#else
#define BUILD 0x0010
#define BUILD_TIME 0x0ED03CD6
#define BUILD_DATE "Nov 16 2007 11:34:48"
#endif
#else
#define PRODUCT "Mobile Station 2 MFx Motorola"
#if MS1_ARTICLE == 60651
#define BUILD 0x0012
#define BUILD_TIME 0x0ED03CE6
#define BUILD_DATE "Nov 16 2007 11:35:04"
#else
#define BUILD 0x0014
#define BUILD_TIME 0x0ED03CF3
#define BUILD_DATE "Nov 16 2007 11:35:17"
#endif
#endif

/* 60651: hardware code 0xC0.., 60652: 0x40.. */
#if MS1_ARTICLE == 60651
#define ARTICLE "60651"
#define HW_CODE (0x8000 | MS1_RELEASE)
#else
#define ARTICLE "60652"
#define HW_CODE MS1_RELEASE
#endif

const struct ms1_ident ms1_ident __attribute__((section(".ident"))) = {
    0, MS1_RELEASE, 0x0108, BUILD, BUILD_TIME, 0, HW_CODE,
    { 0, 0, 0, 0, 0, 0, 0, 0 },
    T(PRODUCT), T(BUILD_DATE), T("Peter Schaeffer"), T("(c) ESU 2006"),
    { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    T(ARTICLE),
    MS1_RELEASE,
    0
};
