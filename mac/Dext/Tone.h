#pragma once
#include <math.h>
#include <stdint.h>

#define TONE_HZ 1000.0
#define TONE_LEVEL 0.25f

/* Stereo interleaved float sine for sample indices [first, first + frames). */
static inline void tone_fill(float *out, uint64_t first, uint32_t frames, double rate)
{
    for (uint32_t i = 0; i < frames; i++) {
        /* Reduce the index to whole periods first so precision holds over long runs. */
        double cycles = fmod((double)(first + i) * TONE_HZ, rate) / rate;
        float s = TONE_LEVEL * (float)sin(2.0 * M_PI * cycles);
        out[2 * i] = s;
        out[2 * i + 1] = s;
    }
}
