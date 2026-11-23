#include "aclock.h"

/* Critically damped second-order loop, ~4 s at 2 ms chunks: source clocks drift slowly, completions jitter. */
#define A (1.0 / 1024)
#define B (A * A / 4)
#define CLAMP_SAMPLES 48.0 /* 1 ms: one stalled completion barely moves the line */
#define GAP_SAMPLES 960.0  /* 20 ms: beyond completion jitter, below a mode switch's 30 ms hole */
#define PPM_MAX 1000e-6

void aclock_init(struct aclock *c, double ticks_per_sample, uint64_t now)
{
    *c = (struct aclock){.nominal_ticks_per_sample = ticks_per_sample, .ticks_per_sample = ticks_per_sample, .h0 = (double)now};
}

static double host_d(const struct aclock *c, uint64_t s) { return c->h0 + ((double)s - (double)c->s0) * c->ticks_per_sample; }

uint64_t aclock_host(const struct aclock *c, uint64_t s) { return (uint64_t)host_d(c, s); }

uint64_t aclock_sample(const struct aclock *c, uint64_t h)
{
    double s = (double)c->s0 + ((double)h - c->h0) / c->ticks_per_sample;
    return s > 0 ? (uint64_t)(s + 0.5) : 0;
}

uint64_t aclock_arrive(struct aclock *c, uint64_t h, uint32_t n, uint64_t *skipped)
{
    *skipped = 0;
    if (!n) return c->next_sample;
    uint64_t last = c->next_sample + n - 1;
    double e = (double)h - host_d(c, last);
    if (!c->synced || e > GAP_SAMPLES * c->nominal_ticks_per_sample || e < -GAP_SAMPLES * c->nominal_ticks_per_sample) {
        uint64_t now = aclock_sample(c, h);
        uint64_t at = now + 1 > n ? now + 1 - n : 0;
        *skipped = at > c->next_sample ? at - c->next_sample : 0;
        c->next_sample = at;
        c->synced = 1;
        c->rephases++;
        c->next_sample += n;
        return at;
    }
    double lim = CLAMP_SAMPLES * c->nominal_ticks_per_sample;
    if (e > lim) e = lim;
    if (e < -lim) e = -lim;
    c->h0 = host_d(c, last) + A * e;
    c->s0 = last;
    c->ticks_per_sample += B * e / n;
    if (c->ticks_per_sample > c->nominal_ticks_per_sample * (1 + PPM_MAX)) c->ticks_per_sample = c->nominal_ticks_per_sample * (1 + PPM_MAX);
    if (c->ticks_per_sample < c->nominal_ticks_per_sample * (1 - PPM_MAX)) c->ticks_per_sample = c->nominal_ticks_per_sample * (1 - PPM_MAX);
    uint64_t at = c->next_sample;
    c->next_sample += n;
    return at;
}
