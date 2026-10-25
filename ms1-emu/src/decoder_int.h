/* Shared by the decoder implementations: decoder.c (the front), decoder_mm.c, decoder_mfx.c. */
#ifndef DECODER_INT_H
#define DECODER_INT_H

#include "decoder.h"

typedef struct {
	double min, max, sum;
	unsigned long n;
} stat_t;

void stat_add(stat_t *s, double v);
/* "  WHAT  min-max UNIT, mean M (N)", or "-" without values */
void stat_print(FILE *f, const char *what, const stat_t *s, const char *unit);

typedef struct {
	void (*track)(decoder_t *d, double t, int polarity);
	void (*status)(const decoder_t *d, char *buf, size_t size);
	void (*report)(decoder_t *d, double t, FILE *out);
	void (*free)(decoder_t *d);
} decoder_ops_t;

struct decoder {
	const decoder_ops_t *ops;
	char name[32];
	FILE *log;               /* events; NULL: none */
	decoder_feedback_t fb;   /* the board's feedback receiver; callbacks NULL without one */
	void *impl;
};

/* An event line: "TIME NAME TEXT" */
void decoder_say(decoder_t *d, double t, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Set up d->ops, d->impl and d->name from the text after "mm:" / "mfx:"; false with a
 * message in err. */
bool mm_create(decoder_t *d, const char *args, char *err, size_t errsize);
bool mfx_create(decoder_t *d, const char *args, char *err, size_t errsize);

#endif
