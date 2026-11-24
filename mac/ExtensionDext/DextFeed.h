#ifndef HD60S_DEXT_FEED_H
#define HD60S_DEXT_FEED_H

#include <stddef.h>
#include <stdint.h>

#include "device.h"
#include "framering.h"

enum { DEXT_FRAME_BYTES = FR_SLOT_BYTES };

/* Camera side of the driver extension. dext_feed_maintain connects, reconnects and disconnects, and is the only
 * call that makes IOKit requests; call it and dext_feed_close from one serial queue. The readers below never wait on
 * it, so a driver that is going away can't stall the caller's video. */
void dext_feed_maintain(void);
/* Copies a frame newer than the last one into out and returns 1, else 0. */
int dext_feed_poll(uint8_t *out, size_t cap, struct fr_frame *info);
/* Maintains the connection, then reads the newest frame's size and number and the driver's counters without copying
 * the frame. 0 when not connected; info and number are zero until a frame arrives. */
int dext_feed_peek(struct fr_frame *info, struct fr_counters *counters, uint32_t *number);
struct fr_counters dext_feed_counters(void);
/* Times the driver connection has come up, so a caller can resend state after a driver restart. */
unsigned dext_feed_connections(void);
void dext_feed_close(void);
/* Sends settings to the driver (user-client method 0, a hd60s_settings_wire) on a connection of its own, reopened if the driver restarted.
 * Used by the app and the camera extension. 0, or -1 when the driver isn't there or refuses them. */
int dext_send_settings(const struct hd60s_settings *s);

#endif
