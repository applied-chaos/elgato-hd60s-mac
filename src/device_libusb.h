#ifndef HD60S_DEVICE_LIBUSB_H
#define HD60S_DEVICE_LIBUSB_H

#include <signal.h>

#include "device.h"

/* Set to silence hd60s_open's stderr messages (status reads open_err instead). */
extern int hd60s_quiet;

/* How long hd60s_open keeps retrying while another process holds the device (0 = fail at once).
 * Clients that must get the device set this so a brief hold, such as the app's status sample, never fails them. */
extern int hd60s_open_wait_ms;

int hd60s_open(struct hd60s *d);
void hd60s_close(struct hd60s *d);
/* Blocks until *stop is set, a transfer fails (returns -1, *stop untouched), or seconds elapse (0 = no limit).
 * Video flows while *want is set (NULL: always) and the source is not HDCP; otherwise passthrough only. */
int hd60s_stream(struct hd60s *d, struct hd60s_parser *p, volatile sig_atomic_t *stop,
                 const volatile sig_atomic_t *want, double seconds);

#endif
