/* Locomotive decoders attached to the MS1's track output (docs/decoders.md). */
#ifndef DECODER_H
#define DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct decoder decoder_t;

/* What a decoder sends back over the track (mfx feedback), as the MS1's RDS receiver passes
 * it on. The callbacks act at the current emulated time: decoders call them from
 * decoder_track. */
typedef struct {
	void *ctx;
	void (*carrier)(void *ctx, bool on);   /* the 52.63 kHz feedback carrier starts / stops */
	void (*bit)(void *ctx, int bit);       /* a data-feedback bit, demodulated with its clock */
} decoder_feedback_t;

/* A decoder from its specification, logging its events to log (NULL: none), answering
 * through fb (NULL: no feedback path):
 *   mm:ADDR[,mm1][,packets]   a Märklin Motorola locomotive decoder at address ADDR (1-80);
 *                             mm1: an old decoder (speed from the first pulse of each pair,
 *                             reverse packets toggle); packets: log every command it takes
 *   mfx:UID[,name=NAME][,symbol=SYMBOL][,cv=VVV/II:XX]...[,noread][,frames]
 *                             an mfx locomotive decoder with the 32-bit UID (hex); NAME,
 *                             SYMBOL (elok, diesel, steam, none) and cv= (hex) set its
 *                             configuration variables; noread: no answer to CV reads;
 *                             frames: log every frame on the track
 * NULL with a message in err when the specification is bad. */
decoder_t *decoder_create(const char *spec, FILE *log, const decoder_feedback_t *fb, char *err, size_t errsize);
void decoder_free(decoder_t *d);

/* The track voltage changed at t seconds: +1, -1, or 0 (no voltage). */
void decoder_track(decoder_t *d, double t, int polarity);

/* One line on the decoder's current state. */
void decoder_status(const decoder_t *d, char *buf, size_t size);

/* End of the run at t: the final state and the analysis of the signal to out. */
void decoder_report(decoder_t *d, double t, FILE *out);

#endif
