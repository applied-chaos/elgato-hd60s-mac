#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/aclock.h"

#define P_NOM (1e9 / 48000)
#define MS 1000000.0
#define T0 1000000000000ull

struct src {
    double true_ns_per_sample;
    uint64_t made;
};

static void src_init(struct src *s, double ppm)
{
    s->true_ns_per_sample = P_NOM / (1 + ppm * 1e-6);
    s->made = 0;
}

static double src_time(const struct src *s, uint64_t k) { return T0 + (double)k * s->true_ns_per_sample; }

static double jitter(double max_ms) { return max_ms * MS * rand() / (double)RAND_MAX; }

struct run {
    uint64_t first_src, placed;
    uint64_t skipped;
};

static struct run feed(struct aclock *c, struct src *s, double until, double late_ms, int *contiguous)
{
    struct run r = {0};
    uint64_t next = 0;
    int have = 0;
    for (;;) {
        uint64_t end = s->made + 96;
        double done = src_time(s, end - 1) + jitter(late_ms);
        if (done > until) break;
        r.first_src = s->made;
        r.placed = aclock_arrive(c, (uint64_t)done, 96, &r.skipped);
        if (contiguous && have && r.placed != next) *contiguous = 0;
        next = r.placed + 96;
        have = 1;
        s->made = end;
    }
    return r;
}

static double line_err_ms(const struct aclock *c, const struct src *s, const struct run *r)
{
    uint64_t k = r->first_src + 95;
    return ((double)aclock_host(c, r->placed + 95) - src_time(s, k)) / MS;
}

static void test_steady(void)
{
    struct aclock c;
    struct src s;
    src_init(&s, 0);
    aclock_init(&c, P_NOM, T0 - 5 * MS);
    int contiguous = 1;
    struct run r = feed(&c, &s, T0 + 10e9, 0, &contiguous);
    assert(contiguous);
    assert(c.rephases == 1);
    assert(fabs(line_err_ms(&c, &s, &r)) < 0.01);
    assert(fabs(c.ticks_per_sample / P_NOM - 1) < 1e-6);
}

static void test_drift(double ppm)
{
    struct aclock c;
    struct src s;
    src_init(&s, ppm);
    aclock_init(&c, P_NOM, T0);
    int contiguous = 1;
    struct run r = feed(&c, &s, T0 + 60e9, 0, &contiguous);
    assert(contiguous);
    assert(c.rephases == 1);
    assert(fabs(line_err_ms(&c, &s, &r)) < 0.05);
    assert(fabs(c.ticks_per_sample / s.true_ns_per_sample - 1) < 2e-6);
}

/* Measured on the device: completion jitter ~0.1 ms, tail to 2.4 ms; one burst of 680 frames after a 14 ms stall. */
static void test_jitter(void)
{
    struct aclock c;
    struct src s;
    src_init(&s, -10);
    aclock_init(&c, P_NOM, T0);
    int contiguous = 1;
    feed(&c, &s, T0 + 5e9, 0.5, &contiguous);
    uint64_t next = c.next_sample;
    double ref = src_time(&s, s.made + 679);
    uint64_t skipped, at = aclock_arrive(&c, (uint64_t)(ref + 0.3 * MS), 680, &skipped);
    assert(at == next && skipped == 0);
    s.made += 680;
    struct run r = feed(&c, &s, T0 + 30e9, 2.4, &contiguous);
    assert(contiguous);
    assert(c.rephases == 1);
    double err = line_err_ms(&c, &s, &r);
    assert(err > 0 && err < 2.4);
    assert(fabs(c.ticks_per_sample / s.true_ns_per_sample - 1) < 20e-6);
}

/* Source mode switch: ~30 ms of samples never arrive, then real time again. */
static void test_gap(void)
{
    struct aclock c;
    struct src s;
    src_init(&s, 50);
    aclock_init(&c, P_NOM, T0);
    int contiguous = 1;
    feed(&c, &s, T0 + 10e9, 0.2, &contiguous);
    assert(contiguous);
    uint64_t before = c.next_sample, probe = c.next_sample + 48000;
    uint64_t h_probe = aclock_host(&c, probe);
    s.made += 1440;
    struct run r = feed(&c, &s, src_time(&s, s.made + 95) + 1, 0, NULL);
    assert(c.rephases == 2);
    assert(r.placed >= before + 1440 - 10 && r.placed <= before + 1440 + 10);
    assert(r.skipped == r.placed - before);
    assert(llabs((long long)aclock_host(&c, probe) - (long long)h_probe) < 20000);
    contiguous = 1;
    r = feed(&c, &s, T0 + 20e9, 0.2, &contiguous);
    assert(contiguous && c.rephases == 2);
    assert(fabs(line_err_ms(&c, &s, &r)) < 0.2);
}

static void test_signal_loss(void)
{
    struct aclock c;
    struct src s;
    src_init(&s, -10);
    aclock_init(&c, P_NOM, T0);
    feed(&c, &s, T0 + 20e9, 0.2, NULL);
    double p = c.ticks_per_sample;
    uint64_t before = c.next_sample;
    s.made += 8 * 48000;
    int contiguous = 1;
    struct run r = feed(&c, &s, src_time(&s, s.made + 95) + 1, 0, NULL);
    assert(c.rephases == 2);
    assert(r.skipped == r.placed - before);
    assert(r.skipped > 8 * 48000 - 20 && r.skipped < 8 * 48000 + 20);
    assert(c.ticks_per_sample == p);
    r = feed(&c, &s, T0 + 40e9, 0.2, &contiguous);
    assert(contiguous && c.rephases == 2);
    assert(fabs(line_err_ms(&c, &s, &r)) < 0.2);
}

static void test_rate_out_of_range(void)
{
    struct aclock c;
    struct src s;
    src_init(&s, 5000);
    aclock_init(&c, P_NOM, T0);
    uint64_t prev = 0;
    for (int i = 0; i < 600; i++) {
        feed(&c, &s, T0 + (i + 1) * 0.1e9, 0.1, NULL);
        uint64_t h = aclock_host(&c, 1000000);
        assert(i == 0 || llabs((long long)h - (long long)prev) < 0.2 * MS);
        prev = h;
    }
    assert(c.ticks_per_sample >= c.nominal_ticks_per_sample * (1 - 1000e-6) && c.ticks_per_sample < c.nominal_ticks_per_sample * (1 - 500e-6));
    assert(c.rephases > 1 && c.rephases < 600);
}

int main(void)
{
    srand(5);
    test_steady();
    test_drift(200);
    test_drift(-200);
    test_jitter();
    test_gap();
    test_signal_loss();
    test_rate_out_of_range();
    puts("test_aclock: ok");
    return 0;
}
