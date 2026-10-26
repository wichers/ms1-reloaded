/* irqstat: interrupt timing of a run (ms1emu -I, difftest -I) */
#ifndef IRQSTAT_H
#define IRQSTAT_H

#include <stdio.h>

#include "mcu.h"

/* Time every interrupt of m from its entry to its RTI (nested ones included); for the track
 * signal interrupt (timer channel 4) also its latency, its slack and the fill of the staging
 * ring it reads. Uses m->step_hook. */
void irqstat_attach(mcu_t *m);
void irqstat_print(mcu_t *m, FILE *out);

#endif
