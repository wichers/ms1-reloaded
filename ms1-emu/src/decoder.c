/* Locomotive decoders on the MS1's track output: the protocol-independent front.
 *
 * A decoder sees the track voltage the H-bridge makes of PT4/PT5 (+, -, or 0 while both sides
 * are equal) and decodes it from the edge timing, as the decoder in a locomotive would. It
 * logs what it does and, at the end, an analysis of the signal. The protocols are in
 * decoder_mm.c (Märklin Motorola) and decoder_mfx.c (mfx). */
#include "decoder_int.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

void stat_add(stat_t *s, double v)
{
	if (!s->n || v < s->min)
		s->min = v;
	if (!s->n || v > s->max)
		s->max = v;
	s->sum += v;
	s->n++;
}

void stat_print(FILE *f, const char *what, const stat_t *s, const char *unit)
{
	if (s->n)
		fprintf(f, "  %-24s %.1f-%.1f %s, mean %.1f (%lu)\n", what, s->min, s->max, unit, s->sum / s->n, s->n);
	else
		fprintf(f, "  %-24s -\n", what);
}

void decoder_say(decoder_t *d, double t, const char *fmt, ...)
{
	va_list ap;
	if (!d->log)
		return;
	va_start(ap, fmt);
	fprintf(d->log, "%10.6f %s ", t, d->name);
	vfprintf(d->log, fmt, ap);
	fputc('\n', d->log);
	va_end(ap);
}

decoder_t *decoder_create(const char *spec, FILE *log, const decoder_feedback_t *fb, char *err, size_t errsize)
{
	decoder_t *d = calloc(1, sizeof *d);
	d->log = log;
	if (fb)
		d->fb = *fb;
	bool ok;
	if (!strncmp(spec, "mm:", 3))
		ok = mm_create(d, spec + 3, err, errsize);
	else if (!strncmp(spec, "mfx:", 4))
		ok = mfx_create(d, spec + 4, err, errsize);
	else {
		snprintf(err, errsize, "decoder '%s': the kinds are mm:ADDR and mfx:UID", spec);
		ok = false;
	}
	if (!ok) {
		free(d);
		return NULL;
	}
	return d;
}

void decoder_free(decoder_t *d)
{
	if (d) {
		d->ops->free(d);
		free(d);
	}
}

void decoder_track(decoder_t *d, double t, int polarity)
{
	d->ops->track(d, t, polarity);
}

void decoder_status(const decoder_t *d, char *buf, size_t size)
{
	d->ops->status(d, buf, size);
}

void decoder_report(decoder_t *d, double t, FILE *out)
{
	d->ops->report(d, t, out);
}
