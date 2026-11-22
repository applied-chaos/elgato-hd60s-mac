#include "deinterlace.h"

#include <stdlib.h>
#include <string.h>

/* Digital source, so no noise floor; only chroma rounding to absorb. */
#define MOTION_THRESHOLD 12

static int moved(const uint8_t *cur, const uint8_t *prev)
{
    for (int i = 0; i < 4; i++) {
        int diff = cur[i] - prev[i];
        if (diff > MOTION_THRESHOLD || diff < -MOTION_THRESHOLD)
            return 1;
    }
    return 0;
}

void deint_frame(struct deint *d, uint8_t *frame, int width, int height)
{
    size_t row = (size_t)width * 2, len = row * (size_t)height;
    int have_prev = d->len == len;
    if (d->cap < len) {
        uint8_t *p = realloc(d->prev, len), *n = realloc(d->next, len);
        if (p)
            d->prev = p;
        if (n)
            d->next = n;
        if (!p || !n)
            return;
        d->cap = len;
    }
    memcpy(d->next, frame, len);
    /* Field 1 (odd lines) is later in time; where it or its neighbours moved, rebuild it from field 0. */
    for (int y = 1; have_prev && y < height; y += 2) {
        uint8_t *line = frame + y * row;
        const uint8_t *above = line - row, *below = y + 1 < height ? line + row : above;
        const uint8_t *pl = d->prev + y * row;
        for (size_t x = 0; x < row; x += 4) {
            const uint8_t *pa = pl - row, *pb = y + 1 < height ? pl + row : pa;
            if (moved(line + x, pl + x) || moved(above + x, pa + x) || moved(below + x, pb + x))
                for (int i = 0; i < 4; i++)
                    line[x + i] = (uint8_t)((above[x + i] + below[x + i] + 1) / 2);
        }
    }
    uint8_t *t = d->prev;
    d->prev = d->next;
    d->next = t;
    d->len = len;
}

void deint_free(struct deint *d)
{
    free(d->prev);
    free(d->next);
    memset(d, 0, sizeof *d);
}
