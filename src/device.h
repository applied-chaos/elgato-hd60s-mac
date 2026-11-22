#ifndef HD60S_DEVICE_H
#define HD60S_DEVICE_H

#include <stdint.h>

#include "parser.h"
#include "tx.h"

#define HD60S_VID 0x0fd9
#define HD60S_PID 0x0074
#define HD60S_EP_VIDEO 0x83

/* Transport seam: libusb on the Mac, IOUSBHostInterface in the driver extension. */
struct hd60s_usb {
    void *ctx;
    /* Vendor control request (bmRequestType 0xC0 in / 0x40 out). Bytes moved, or < 0 on failure. */
    int (*ctrl)(void *ctx, int in, uint8_t req, uint16_t val, uint16_t idx, uint8_t *buf, uint16_t len);
    void (*sleep_ms)(void *ctx, int ms);
};

struct libusb_context;
struct libusb_device_handle;

struct hd60s {
    struct hd60s_usb usb;
    struct libusb_context *lusb;
    struct libusb_device_handle *h;
    int open_err;          /* libusb result of the open/claim steps: 0, or why they failed */
    int err;               /* first transport error seen, 0 if none */
    uint32_t mcu_version;  /* b0<<16 | b1<<8 | b2 from MCU command 0x58 */
    uint8_t mst_saved[5];  /* MST3367 bank 1 reg 0x27 snapshot, restored on stop */
    int started;
    int sd;                /* colour matrix applied: 1 = BT.601, 0 = BT.709 */
    struct tx tx;          /* HDMI-out transmitter state */
};

/* Periodic work while streaming: aspect, colour matrix, HDMI-out mode. */
struct hd60s_policy {
    long win_start, last_poll, aspect_at;
    int w, h, interlaced, frames, run; /* frame size must hold for 3/4 of the window's frames */
    const struct tx_mode *mode;
};

/* usb must be set. Wakes the MCU and finds its bridge. */
int hd60s_init(struct hd60s *d);
int hd60s_start(struct hd60s *d);
void hd60s_stop(struct hd60s *d);

void hd60s_policy_begin(struct hd60s_policy *s, long now_ms);
void hd60s_policy_frame(struct hd60s_policy *s, int w, int h, int interlaced);
/* Issues control requests, so call it off the bulk completion path. */
void hd60s_policy_tick(struct hd60s *d, struct hd60s_parser *p, struct hd60s_policy *s, long now_ms);

int mst_read(struct hd60s *d, uint8_t bank, uint8_t reg);
int i2c_read(struct hd60s *d, uint8_t addr, uint8_t sub);

#endif
