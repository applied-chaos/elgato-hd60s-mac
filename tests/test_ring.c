#include "../src/ring.h"

#include <assert.h>
#include <stdio.h>

static void push_ramp(struct ring *r, int start, size_t frames)
{
    int16_t pcm[2 * 64];
    for (size_t i = 0; i < frames; i++) {
        pcm[2 * i] = (int16_t)(start + (int)i);
        pcm[2 * i + 1] = (int16_t)-(start + (int)i);
    }
    ring_push(r, pcm, frames);
}

int main(void)
{
    float out[2 * 64];
    struct ring *r = ring_create(32);

    /* Silent until the target fill is reached. */
    push_ramp(r, 1, 7);
    assert(ring_pull(r, out, 4, 8) == 0);
    assert(out[0] == 0 && out[7] == 0);

    /* Primed: samples come out in order, scaled to [-1, 1), left then right. */
    push_ramp(r, 8, 1);
    assert(ring_pull(r, out, 4, 8) == 4);
    assert(out[0] == 1 / 32768.0f && out[1] == -1 / 32768.0f && out[6] == 4 / 32768.0f);

    /* Underrun: what is left plays, the rest is silence, and the ring re-primes. */
    assert(ring_pull(r, out, 8, 8) == 4);
    assert(out[6] == 8 / 32768.0f && out[8] == 0 && out[15] == 0);
    push_ramp(r, 100, 4);
    assert(ring_pull(r, out, 4, 8) == 0);

    /* Too much buffered: skip ahead so only the target is left, across the wrap. */
    push_ramp(r, 200, 28);
    assert(ring_pull(r, out, 2, 4) == 2);
    assert(out[0] == 224 / 32768.0f);

    /* A push that does not fit is dropped rather than overwriting unread audio. */
    struct ring *small = ring_create(8);
    push_ramp(small, 1, 6);
    push_ramp(small, 50, 6);
    assert(ring_pull(small, out, 6, 2) == 6);
    assert(out[10] == 6 / 32768.0f);
    ring_free(small);

    ring_free(r);
    puts("ring tests passed");
    return 0;
}
