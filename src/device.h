#ifndef HD60S_DEVICE_H
#define HD60S_DEVICE_H

#include <libusb.h>
#include <signal.h>
#include <stdint.h>

#include "parser.h"

#define HD60S_VID 0x0fd9
#define HD60S_PID 0x0074
#define HD60S_EP_VIDEO 0x83

struct hd60s {
    libusb_context *usb;
    libusb_device_handle *h;
    int err;               /* first libusb error seen, 0 if none */
    uint32_t mcu_version;  /* b0<<16 | b1<<8 | b2 from MCU command 0x58 */
    uint8_t mst_saved[5];  /* MST3367 bank 1 reg 0x27 snapshot, restored on stop */
    int started;
};

int hd60s_open(struct hd60s *d);
int hd60s_start(struct hd60s *d);
void hd60s_stop(struct hd60s *d);
void hd60s_close(struct hd60s *d);
/* Blocks until *stop is set, a transfer fails, or seconds elapse (0 = no limit). */
int hd60s_stream(struct hd60s *d, struct hd60s_parser *p, volatile sig_atomic_t *stop, double seconds);

int mst_read(struct hd60s *d, uint8_t bank, uint8_t reg);
int i2c_read(struct hd60s *d, uint8_t addr, uint8_t sub);

#endif
