#ifndef HD60S_DEVICE_H
#define HD60S_DEVICE_H

#include <stdint.h>

#include "audio.h"
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

struct hd60s_colour_cfg {
    enum hd60s_input_format input;
    int sd;
};

/* MST3367 CSC: bank 0 reg 0x92 mode and the 12 words written from reg 0x93. */
struct hd60s_csc {
    uint8_t mode;
    uint16_t m[12];
};

/* What the source's AVI InfoFrame says: pixel format (b2:0x48 Y1Y0) and colorimetry (b2:0x49 C1C0). */
struct hd60s_source {
    enum hd60s_input_format format;
    enum hd60s_colour colorimetry; /* COLOUR_UNKNOWN when unspecified or extended */
};

struct hd60s_source hd60s_parse_avi(int present, uint8_t db1, uint8_t db2);
void hd60s_csc_plan(const struct hd60s_colour_cfg *c, struct hd60s_csc *out);
/* Matrix of the frames leaving the device: its own for RGB input, else the source's with a size fallback. */
enum hd60s_colour hd60s_frame_colour(const struct hd60s_colour_cfg *c, enum hd60s_colour colorimetry);

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
    struct hd60s_colour_cfg colour;
    struct tx tx;          /* HDMI-out transmitter state */
    struct audio_settings audio;
    int audio_routed_dvi;
};

/* Periodic work while streaming: aspect, colour matrix, HDMI-out mode. */
struct hd60s_policy {
    long win_start, last_poll, aspect_at;
    struct hd60s_source src;
    int w, h, interlaced, frames, run; /* frame size must hold for 3/4 of the window's frames */
    const struct tx_mode *mode;
    int hz, hz_seen; /* source audio rate, accepted once two polls agree */
};

/* usb must be set. Wakes the MCU and finds its bridge. Resets audio to AUDIO_DEFAULT: call hd60s_set_audio after. */
int hd60s_init(struct hd60s *d);
int hd60s_start(struct hd60s *d);
void hd60s_stop(struct hd60s *d);
/* Gain is clamped to +-12 dB. Sent now if started, otherwise at hd60s_start. 0, or -1 on a transport error. */
int hd60s_set_audio(struct hd60s *d, const struct audio_settings *s);

void hd60s_policy_begin(struct hd60s_policy *s, long now_ms);
void hd60s_policy_frame(struct hd60s_policy *s, int w, int h, int interlaced);
/* Issues control requests, so call it off the bulk completion path. */
void hd60s_policy_tick(struct hd60s *d, struct hd60s_parser *p, struct hd60s_policy *s, long now_ms);

int mst_read(struct hd60s *d, uint8_t bank, uint8_t reg);
/* Source audio rate from the receiver's ACR packet for the given video mode; 0 if no audio or no mode. */
int hd60s_source_audio_hz(struct hd60s *d, const struct tx_mode *m);
/* Vsync period counter for hd60s_rate_from_vsync; 0 if unreadable. */
unsigned hd60s_vsync_counter(struct hd60s *d);
int i2c_read(struct hd60s *d, uint8_t addr, uint8_t sub);

#endif
