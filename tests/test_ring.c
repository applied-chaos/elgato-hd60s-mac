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

    /* Silent until the target plus one pull is buffered. */
    push_ramp(r, 1, 11);
    assert(ring_pull(r, out, 4, 8) == 0);
    assert(out[0] == 0 && out[7] == 0);

    /* Primed: samples come out in order, scaled to [-1, 1), left then right. */
    push_ramp(r, 12, 1);
    assert(ring_pull(r, out, 4, 8) == 4);
    assert(out[0] == 1 / 32768.0f && out[1] == -1 / 32768.0f && out[6] == 4 / 32768.0f);

    /* Underrun: what is left plays, the rest is silence, and the ring re-primes. */
    assert(ring_pull(r, out, 16, 8) == 8);
    assert(out[0] == 5 / 32768.0f && out[14] == 12 / 32768.0f && out[16] == 0 && out[31] == 0);
    push_ramp(r, 100, 4);
    assert(ring_pull(r, out, 4, 8) == 0);

    /* Too much buffered: skip ahead so the target is left after the pull, across the wrap. */
    push_ramp(r, 200, 28);
    assert(ring_pull(r, out, 2, 4) == 2);
    assert(out[0] == 222 / 32768.0f);

    /* Latency stays put: jitter within a quarter of the target is kept, but the extra a late burst leaves after
     * an underrun is trimmed rather than delaying everything after it. */
    struct ring *steady = ring_create(256);
    push_ramp(steady, 1, 40);
    assert(ring_pull(steady, out, 8, 32) == 8); /* 32 left */
    push_ramp(steady, 41, 8 + 7);
    assert(ring_pull(steady, out, 8, 32) == 8 && out[0] == 9 / 32768.0f); /* 39 left: jitter kept */
    push_ramp(steady, 56, 8 + 20);
    assert(ring_pull(steady, out, 8, 32) == 8 && out[0] == 44 / 32768.0f); /* 67 queued: trimmed to 32 after */
    ring_free(steady);

    /* Priming starts at the same latency however the first audio arrives: one big burst is cut back to it. */
    struct ring *burst = ring_create(256);
    push_ramp(burst, 1, 30);
    assert(ring_pull(burst, out, 8, 32) == 0); /* under target + pull: still silent */
    push_ramp(burst, 31, 30);
    assert(ring_pull(burst, out, 8, 32) == 8 && out[0] == 21 / 32768.0f); /* 60 queued: starts 40 back */
    assert(ring_fill(burst) == 32);
    ring_free(burst);

    /* A push that does not fit is dropped rather than overwriting unread audio. */
    struct ring *small = ring_create(8);
    push_ramp(small, 1, 6);
    push_ramp(small, 50, 6);
    assert(ring_pull(small, out, 6, 0) == 6);
    assert(out[10] == 6 / 32768.0f);
    ring_free(small);

    static _Alignas(16) unsigned char mem[1024];
    assert(ring_bytes(8) <= sizeof mem);
    struct ring *own = ring_init(mem, 8);
    push_ramp(own, 1, 2);
    assert(ring_pull(own, out, 2, 0) == 2 && out[2] == 2 / 32768.0f);

    ring_free(r);
    puts("ring tests passed");
    return 0;
}
