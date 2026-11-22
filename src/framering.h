#ifndef HD60S_FRAMERING_H
#define HD60S_FRAMERING_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "link.h"

/* Video frames from the driver extension to the camera extension, in memory the camera maps read-only.
 * One writer; per-slot seqlock so a reader detects a slot rewritten under it. */
#define FR_MAGIC 0x48443646u /* "HD6F" */
#define FR_VERSION 2u
#define FR_SLOTS 3
#define FR_SLOT_BYTES (1920u * 1440u * 2u)

struct fr_slot {
    _Atomic uint32_t seq; /* frame number held, 0 while being written */
    int32_t width, height, interlaced, wide, colour;
    uint32_t len;
};

struct fr_header {
    uint32_t magic, version;
    _Atomic uint32_t latest; /* newest complete frame number, 0 = none */
    _Atomic uint32_t link;   /* enum link_state of the USB session */
    _Atomic uint64_t frames, discarded, long_lines, errors;
    _Atomic uint32_t mcu_version, top_first;
    struct fr_slot slot[FR_SLOTS];
};

struct fr_frame {
    int width, height, interlaced, wide, colour;
    size_t len;
};

struct fr_counters {
    enum link_state link;
    uint64_t frames, discarded, long_lines, errors;
    uint32_t mcu_version, top_first;
};

size_t fr_bytes(void);
struct fr_header *fr_init(void *mem); /* mem holds fr_bytes() */
int fr_valid(const struct fr_header *h, size_t mapped);
/* Drops frames that do not fit a slot. */
void fr_publish(struct fr_header *h, const uint8_t *frame, int width, int height, int interlaced, int wide, int colour);
/* Copies the newest frame if it is newer than seen. Returns its number, or 0 (none new, torn, or does not fit). */
uint32_t fr_take(const struct fr_header *h, uint32_t seen, uint8_t *out, size_t cap, struct fr_frame *info);

/* Size of the newest complete frame without copying it. Returns its number, or 0. */
uint32_t fr_latest(const struct fr_header *h, struct fr_frame *info);

void fr_status(struct fr_header *h, enum link_state link, uint64_t frames, uint64_t discarded, uint64_t long_lines,
               uint64_t errors);
/* top_first: last interlaced frame arrived even field first. */
void fr_device(struct fr_header *h, uint32_t mcu_version, int top_first);
struct fr_counters fr_counters(const struct fr_header *h);

#endif
