#ifndef HD60S_DEXT_FEED_H
#define HD60S_DEXT_FEED_H

#include <stddef.h>
#include <stdint.h>

#include "framering.h"

enum { DEXT_FRAME_BYTES = FR_SLOT_BYTES };

/* Camera side of the driver extension: reconnects on its own. Call from one queue at the output rate.
 * Copies a frame newer than the last one into out and returns 1, else 0. */
int dext_feed_poll(uint8_t *out, size_t cap, struct fr_frame *info);
/* Newest frame's size and the driver's counters, without copying the frame. 0 when not connected; info is zeroed until a frame arrives. */
int dext_feed_peek(struct fr_frame *info, struct fr_counters *counters);
struct fr_counters dext_feed_counters(void);
void dext_feed_close(void);

#endif
