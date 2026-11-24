#ifndef HD60S_RING_H
#define HD60S_RING_H

#include <stddef.h>
#include <stdint.h>

/* Single-producer single-consumer jitter buffer of interleaved stereo s16 frames. */
struct ring;

struct ring *ring_create(size_t frames);
/* For callers without malloc: mem holds ring_bytes(frames), suitably aligned. */
size_t ring_bytes(size_t frames);
struct ring *ring_init(void *mem, size_t frames);
void ring_free(struct ring *r);
/* Producer: drops the whole push if it does not fit. */
void ring_push(struct ring *r, const int16_t *pcm, size_t frames);
/* Consumer: fills out with frames of float stereo, padding with silence. Holds silence until target plus this
 * pull is buffered and starts from exactly that, so latency doesn't depend on how the first audio arrived; then
 * keeps it there: when more than a quarter of target extra builds up
 * (a late burst after an underrun, or clock drift), skips ahead so target frames are left after this pull.
 * Returns the number of real frames written. */
size_t ring_pull(struct ring *r, float *out, size_t frames, size_t target);
/* Frames buffered now. */
size_t ring_fill(struct ring *r);

#endif
