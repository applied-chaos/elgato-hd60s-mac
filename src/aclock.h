#ifndef HD60S_ACLOCK_H
#define HD60S_ACLOCK_H

#include <stdint.h>

/* HDMI audio sample clock in host time, estimated from USB completions.
 * host(s) = h0 + (s - s0) * p; the line never jumps, only chunk placement does. */
struct aclock {
    double p_nom, p; /* host ticks per sample */
    double h0;
    uint64_t s0;
    uint64_t wr;     /* sample index of the next frame from the source */
    int synced;
    unsigned long rephases;
};

/* Starts the line at sample 0 = host time now, at the nominal rate. */
void aclock_init(struct aclock *c, double ticks_per_sample, uint64_t now);
/* n frames completed at host time h. Returns the sample index of the first one. Frames from the
 * previous wr up to that index were never delivered; *skipped gets their count (caller writes silence). */
uint64_t aclock_arrive(struct aclock *c, uint64_t h, uint32_t n, uint64_t *skipped);
uint64_t aclock_host(const struct aclock *c, uint64_t s);
uint64_t aclock_sample(const struct aclock *c, uint64_t h);

#endif
