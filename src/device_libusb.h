#ifndef HD60S_DEVICE_LIBUSB_H
#define HD60S_DEVICE_LIBUSB_H

#include <signal.h>

#include "device.h"

/* Set to silence hd60s_open's stderr messages (status reads open_err instead). */
extern int hd60s_quiet;

int hd60s_open(struct hd60s *d);
void hd60s_close(struct hd60s *d);
/* Blocks until *stop is set, a transfer fails, or seconds elapse (0 = no limit). */
int hd60s_stream(struct hd60s *d, struct hd60s_parser *p, volatile sig_atomic_t *stop, double seconds);

#endif
