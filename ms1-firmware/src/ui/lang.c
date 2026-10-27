/*==========================================================================
 * ui/lang.c - the menu texts
 *
 * Per language (language_index: 0 English, 1 German, 2 French, 3 Dutch) the texts LANG(k)
 * picks, in the display character set: a, o, u and d stand for Ä, Ö, Ü and ß (the font's
 * glyphs), shown in the comments.
 *========================================================================*/

#include "../include/hcs12.h"
#include "../include/rom_tables.h"

#define T(s)    ((const u8 *)(s))

static const u8 *const lang_en[LANG_COUNT] = {   /* English */
    T("NEW LOC"),               /* 0 */
    T("EDIT LOC"),              /* 1 */
    T("DELETE LOC"),            /* 2 */
    T("SETUP"),                 /* 3 */
    T("DATABASE"),              /* 4 */
    T("ADDRESS"),               /* 5 */
    T("CENTRAL"),               /* 6 */
    T("ADDRESS"),               /* 7 */
    T("NAME"),                  /* 8 */
    T("VMAX"),                  /* 9 */
    T("ACC"),                   /* 10 */
    T("DEC"),                   /* 11 */
    T("VOL"),                   /* 12 */
    T("REG"),                   /* 13 */
    T("PRESET"),                /* 14 */
    T("LANGUAGE"),              /* 15 */
    T("INFO"),                  /* 16 */
    T("PRESET"),                /* 17 */
    T("ENGLISH"),               /* 18 */
    T("DEUTSCH"),               /* 19 */
    T("FRANCAIS"),              /* 20 */
    T("NEDERLANDS"),            /* 21 */
    T("PRESET?"),               /* 22 */
    T("NO MASTER"),             /* 23 */
    T("LIST FULL"),             /* 24 */
    T("ADDR. USED"),            /* 25 */
    T("LOC USED"),              /* 26 */
    T("PROG"),                  /* 27 */
    T("NO LOC"),                /* 28 */
    T("UPDATE!"),               /* 29 */
    T("SYMBOL?"),               /* 30 */
    T("DELETE?"),               /* 31 */
    T("PROG LOC?"),             /* 32 */
    T("SAVE"),                  /* 33 */
    T("ERROR"),                 /* 34 */
};

static const u8 *const lang_de[LANG_COUNT] = {   /* German */
    T("NEUE LOK"),              /* 0 */
    T("LOK aNDERN"),            /* 1 */   /* LOK ÄNDERN */
    T("LOK LoSCHEN"),           /* 2 */   /* LOK LÖSCHEN */
    T("OPTIONEN"),              /* 3 */
    T("DATENBANK"),             /* 4 */
    T("ADRESSE"),               /* 5 */
    T("ZENTRALE"),              /* 6 */
    T("ADRESSE"),               /* 7 */
    T("NAME"),                  /* 8 */
    T("VMAX"),                  /* 9 */
    T("ACC"),                   /* 10 */
    T("DEC"),                   /* 11 */
    T("VOL"),                   /* 12 */
    T("REG"),                   /* 13 */
    T("RESET"),                 /* 14 */
    T("SPRACHE"),               /* 15 */
    T("INFO"),                  /* 16 */
    T("RESET"),                 /* 17 */
    T("ENGLISH"),               /* 18 */
    T("DEUTSCH"),               /* 19 */
    T("FRANCAIS"),              /* 20 */
    T("NEDERLANDS"),            /* 21 */
    T("RESET?"),                /* 22 */
    T("KEIN MASTER"),           /* 23 */
    T("LISTE VOLL"),            /* 24 */
    T("ADR.FEHLER"),            /* 25 */
    T("LOK AKTIV"),             /* 26 */
    T("PROG"),                  /* 27 */
    T("KEINE LOK"),             /* 28 */
    T("UPDATE!"),               /* 29 */
    T("SYMBOL?"),               /* 30 */
    T("LoSCHEN?"),              /* 31 */   /* LÖSCHEN? */
    T("PROG LOK?"),             /* 32 */
    T("SPEICHERN"),             /* 33 */
    T("FEHLER"),                /* 34 */
};

static const u8 *const lang_fr[LANG_COUNT] = {   /* French */
    T("NOUV.LOC"),              /* 0 */
    T("MODIF LOC"),             /* 1 */
    T("SUPPR LOC"),             /* 2 */
    T("OPTIONS"),               /* 3 */
    T("BANQUE LOC"),            /* 4 */
    T("ADRESSE"),               /* 5 */
    T("CENTRALE"),              /* 6 */
    T("ADRESSE"),               /* 7 */
    T("NOM"),                   /* 8 */
    T("VMAX"),                  /* 9 */
    T("ACC"),                   /* 10 */
    T("DEC"),                   /* 11 */
    T("VOL"),                   /* 12 */
    T("REG"),                   /* 13 */
    T("RESET"),                 /* 14 */
    T("LANGUE"),                /* 15 */
    T("INFO"),                  /* 16 */
    T("RESET"),                 /* 17 */
    T("ENGLISH"),               /* 18 */
    T("DEUTSCH"),               /* 19 */
    T("FRANCAIS"),              /* 20 */
    T("NEDERLANDS"),            /* 21 */
    T("RESET?"),                /* 22 */
    T("NON MASTER"),            /* 23 */
    T("LISTE PLEINE"),          /* 24 */
    T("ERREUR ADR"),            /* 25 */
    T("LOC ACTIVE"),            /* 26 */
    T("PROG"),                  /* 27 */
    T("PAS DE LOC"),            /* 28 */
    T("UPDATE!"),               /* 29 */
    T("SYMBOLE?"),              /* 30 */
    T("SUPPR?"),                /* 31 */
    T("PROG LOC?"),             /* 32 */
    T("SAUVER"),                /* 33 */
    T("ERREUR"),                /* 34 */
};

static const u8 *const lang_nl[LANG_COUNT] = {   /* Dutch */
    T("NIEUWE LOC"),            /* 0 */
    T("WYZIG LOC"),             /* 1 */
    T("LOC WISSEN"),            /* 2 */
    T("OPTIES"),                /* 3 */
    T("DATABANK"),              /* 4 */
    T("ADRES"),                 /* 5 */
    T("CENTRALE"),              /* 6 */
    T("ADRES"),                 /* 7 */
    T("NAAM"),                  /* 8 */
    T("MAX.SNELH."),            /* 9 */
    T("OPTREKKEN"),             /* 10 */
    T("AFREMMEN"),              /* 11 */
    T("VOL"),                   /* 12 */
    T("REG"),                   /* 13 */
    T("BASIS INST"),            /* 14 */
    T("TAAL"),                  /* 15 */
    T("INFO"),                  /* 16 */
    T("BASIS INST"),            /* 17 */
    T("ENGLISH"),               /* 18 */
    T("DEUTSCH"),               /* 19 */
    T("FRANCAIS"),              /* 20 */
    T("NEDERLANDS"),            /* 21 */
    T("BASIS INST?"),           /* 22 */
    T("GEEN MASTER"),           /* 23 */
    T("LIJST VOL"),             /* 24 */
    T("ADR.INGEBR"),            /* 25 */
    T("LOC INGEBR"),            /* 26 */
    T("PROG"),                  /* 27 */
    T("GEEN LOC"),              /* 28 */
    T("UPDATE!"),               /* 29 */
    T("SYMBOOL?"),              /* 30 */
    T("WISSEN?"),               /* 31 */
    T("PROG LOC?"),             /* 32 */
    T("OPSLAAN"),               /* 33 */
    T("FOUT"),                  /* 34 */
};

const u8 *const *const lang_table[] = { lang_en, lang_de, lang_fr, lang_nl };
