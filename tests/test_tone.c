#include "../mac/Dext/Tone.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
    float out[2 * 8];

    /* Sample 0 is silent; a 48 kHz / 1 kHz tone peaks at sample 12 (quarter period). */
    tone_fill(out, 0, 8, 48000.0);
    assert(out[0] == 0.0f && out[1] == 0.0f);
    tone_fill(out, 12, 1, 48000.0);
    assert(fabsf(out[0] - TONE_LEVEL) < 1e-4f && out[0] == out[1]);

    /* Stateless: filling in two halves matches one fill. */
    float whole[2 * 8], parts[2 * 8];
    tone_fill(whole, 100, 8, 48000.0);
    tone_fill(parts, 100, 3, 48000.0);
    tone_fill(parts + 6, 103, 5, 48000.0);
    for (int i = 0; i < 16; i++)
        assert(whole[i] == parts[i]);

    /* Period of 48 samples repeats exactly, even far into the stream. */
    tone_fill(out, 48000ULL * 3600, 1, 48000.0);
    assert(fabsf(out[0]) < 1e-3f);

    puts("test_tone ok");
    return 0;
}
