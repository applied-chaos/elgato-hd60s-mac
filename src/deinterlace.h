#ifndef HD60S_DEINTERLACE_H
#define HD60S_DEINTERLACE_H

#include <stddef.h>
#include <stdint.h>

struct deint {
    uint8_t *prev, *next; /* previous woven frame, as received */
    size_t cap;
    size_t len; /* bytes of the previous frame held, 0 when none */
};

/* Motion-adaptive, in place, on a woven YUY2 frame (F=0 even lines first). Still areas keep both fields. */
void deint_frame(struct deint *d, uint8_t *frame, int width, int height);
void deint_free(struct deint *d);

#endif
