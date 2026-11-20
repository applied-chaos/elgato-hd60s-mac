#include "ring.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct ring {
    size_t cap;
    _Atomic size_t wr, rd; /* frame counters; only grow */
    int primed;            /* consumer only */
    int16_t pcm[];
};

struct ring *ring_create(size_t frames)
{
    struct ring *r = calloc(1, sizeof *r + frames * 2 * sizeof(int16_t));
    if (r)
        r->cap = frames;
    return r;
}

void ring_free(struct ring *r)
{
    free(r);
}

void ring_push(struct ring *r, const int16_t *pcm, size_t frames)
{
    size_t wr = atomic_load_explicit(&r->wr, memory_order_relaxed);
    size_t rd = atomic_load_explicit(&r->rd, memory_order_acquire);
    if (wr - rd + frames > r->cap)
        return;
    for (size_t i = 0; i < frames; i++)
        memcpy(&r->pcm[(wr + i) % r->cap * 2], &pcm[i * 2], 2 * sizeof(int16_t));
    atomic_store_explicit(&r->wr, wr + frames, memory_order_release);
}

size_t ring_pull(struct ring *r, float *out, size_t frames, size_t target)
{
    size_t wr = atomic_load_explicit(&r->wr, memory_order_acquire);
    size_t rd = atomic_load_explicit(&r->rd, memory_order_relaxed);
    size_t fill = wr - rd;
    if (!r->primed && fill >= target)
        r->primed = 1;
    if (fill > 4 * target) {
        rd = wr - target;
        fill = target;
    }
    size_t n = r->primed ? (fill < frames ? fill : frames) : 0;
    for (size_t i = 0; i < n * 2; i++)
        out[i] = r->pcm[(rd * 2 + i) % (r->cap * 2)] / 32768.0f;
    memset(out + n * 2, 0, (frames - n) * 2 * sizeof(float));
    if (r->primed && n < frames)
        r->primed = 0;
    atomic_store_explicit(&r->rd, rd + n, memory_order_release);
    return n;
}
