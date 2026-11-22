#include "framering.h"

#include <string.h>

#define FR_DATA_OFFSET 4096u

_Static_assert(sizeof(struct fr_header) <= FR_DATA_OFFSET, "header overlaps slot data");

size_t fr_bytes(void) { return FR_DATA_OFFSET + (size_t)FR_SLOTS * FR_SLOT_BYTES; }

static uint8_t *slot_data(const struct fr_header *h, uint32_t i)
{
    return (uint8_t *)h + FR_DATA_OFFSET + (size_t)i * FR_SLOT_BYTES;
}

struct fr_header *fr_init(void *mem)
{
    struct fr_header *h = mem;
    memset(h, 0, sizeof *h);
    h->magic = FR_MAGIC;
    h->version = FR_VERSION;
    atomic_store(&h->link, LINK_DOWN);
    return h;
}

int fr_valid(const struct fr_header *h, size_t mapped)
{
    return mapped >= fr_bytes() && h->magic == FR_MAGIC && h->version == FR_VERSION;
}

void fr_publish(struct fr_header *h, const uint8_t *frame, int width, int height, int interlaced, int wide, int colour)
{
    size_t len = (size_t)width * (size_t)height * 2;
    if (width <= 0 || height <= 0 || len > FR_SLOT_BYTES)
        return;
    uint32_t n = atomic_load_explicit(&h->latest, memory_order_relaxed) + 1;
    if (!n)
        n = 1;
    struct fr_slot *s = &h->slot[n % FR_SLOTS];
    atomic_store_explicit(&s->seq, 0, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    memcpy(slot_data(h, n % FR_SLOTS), frame, len);
    s->width = width;
    s->height = height;
    s->interlaced = interlaced;
    s->wide = wide;
    s->colour = colour;
    s->len = (uint32_t)len;
    atomic_store_explicit(&s->seq, n, memory_order_release);
    atomic_store_explicit(&h->latest, n, memory_order_release);
}

uint32_t fr_take(const struct fr_header *h, uint32_t seen, uint8_t *out, size_t cap, struct fr_frame *info)
{
    uint32_t n = atomic_load_explicit(&h->latest, memory_order_acquire);
    if (!n || n == seen)
        return 0;
    const struct fr_slot *s = &h->slot[n % FR_SLOTS];
    if (atomic_load_explicit(&s->seq, memory_order_acquire) != n)
        return 0;
    struct fr_frame f = {s->width, s->height, s->interlaced, s->wide, s->colour, s->len};
    if (f.width <= 0 || f.height <= 0 || f.len != (size_t)f.width * (size_t)f.height * 2 || f.len > FR_SLOT_BYTES ||
        f.len > cap)
        return 0;
    memcpy(out, slot_data(h, n % FR_SLOTS), f.len);
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&s->seq, memory_order_relaxed) != n)
        return 0;
    *info = f;
    return n;
}

void fr_status(struct fr_header *h, enum link_state link, uint64_t frames, uint64_t discarded, uint64_t long_lines,
               uint64_t errors)
{
    atomic_store_explicit(&h->frames, frames, memory_order_relaxed);
    atomic_store_explicit(&h->discarded, discarded, memory_order_relaxed);
    atomic_store_explicit(&h->long_lines, long_lines, memory_order_relaxed);
    atomic_store_explicit(&h->errors, errors, memory_order_relaxed);
    atomic_store_explicit(&h->link, (uint32_t)link, memory_order_release);
}

struct fr_counters fr_counters(const struct fr_header *h)
{
    uint32_t link = atomic_load_explicit(&h->link, memory_order_acquire);
    return (struct fr_counters){
        link <= LINK_BACKOFF ? (enum link_state)link : LINK_DOWN,
        atomic_load_explicit(&h->frames, memory_order_relaxed),
        atomic_load_explicit(&h->discarded, memory_order_relaxed),
        atomic_load_explicit(&h->long_lines, memory_order_relaxed),
        atomic_load_explicit(&h->errors, memory_order_relaxed),
    };
}
