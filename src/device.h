#ifndef HD60S_DEVICE_H
#define HD60S_DEVICE_H

#include <stddef.h>
#include <stdint.h>

#include "audio.h"
#include "parser.h"
#include "tx.h"

#define HD60S_VID 0x0fd9
#define HD60S_PID 0x0074
#define HD60S_EP_VIDEO 0x83

/* Bulk is the default (iface 0 alt 4); isochronous is alt 1-3 and suits flaky hubs. */
enum hd60s_transfer { HD60S_BULK, HD60S_ISO };

/* Bytes per service interval of EP 0x83 in alt settings 1-3 (0 = absent), which iso picks from. */
#define HD60S_ISO_ALTS 3
struct hd60s_xfer_plan {
    int alt;
    int buffers;
    int packets;      /* iso packets per buffer, 0 for bulk */
    int packet_bytes; /* iso bytes per packet, 0 for bulk */
};

int hd60s_transfer_parse(const char *s, enum hd60s_transfer *out);
#define HD60S_ISO_MIN_BYTES (1920 * 1080 * 2 * 60 / 8000)
/* Iso takes the narrowest alt that carries 1080p60: alt 3 delivers no data on a Mac on a Mac. -1 if none does. */
int hd60s_xfer_plan(enum hd60s_transfer mode, const int alt_bytes[HD60S_ISO_ALTS], struct hd60s_xfer_plan *out);

/* Transport seam: libusb on the Mac, IOUSBHostInterface in the driver extension. */
struct hd60s_usb {
    void *ctx;
    enum hd60s_transfer transfer;
    /* Vendor control request (bmRequestType 0xC0 in / 0x40 out). Bytes moved, or < 0 on failure. */
    int (*ctrl)(void *ctx, int in, uint8_t req, uint16_t val, uint16_t idx, uint8_t *buf, uint16_t len);
    void (*sleep_ms)(void *ctx, int ms);
};

/* Picture controls as offsets from Elgato's defaults (device bytes 0x81, 0x80, 0x80, 0x80), so zero is neutral. */
struct hd60s_picture {
    int8_t brightness, contrast, saturation, hue;
};

/* User settings the device owner applies; the app writes them, the camera extension and dext read them. */
struct hd60s_settings {
    enum hd60s_range range;
    struct hd60s_picture picture;
    struct audio_pref audio;
};

/* Parses {"range":"auto|standard|expanded"} plus optional "brightness", "contrast", "saturation", "hue" integers
 * in -128..127 (absent = 0), "source" hdmi|line|mix, "gain" -24..24 half dB and "mute" true|false (absent = Elgato's
 * default audio); out is untouched on failure. */
int hd60s_settings_parse(const char *json, struct hd60s_settings *out);
void hd60s_settings_print(const struct hd60s_settings *s, char *buf, size_t n);
/* Range for a name as above, or -1. */
int hd60s_range_from_name(const char *name);

struct hd60s_colour_cfg {
    enum hd60s_input_format input;
    int sd;
    enum hd60s_range range;
    int quant; /* AVI RGB quantisation range Q (b2:0x4A bits 3:2): 1 limited, 2 full */
    struct hd60s_picture picture;
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
    int quant;                     /* b2:0x4A bits 3:2 */
};

struct hd60s_source hd60s_parse_avi(int present, uint8_t db1, uint8_t db2, uint8_t db3);
void hd60s_csc_plan(const struct hd60s_colour_cfg *c, struct hd60s_csc *out);
/* Matrix of the frames leaving the device: its own for RGB input, else the source's with a size fallback. */
enum hd60s_colour hd60s_frame_colour(const struct hd60s_colour_cfg *c, enum hd60s_colour colorimetry);

/* Source link flags (FUN_125f7): HDCP encrypted, and DVI rather than HDMI signalling. */
struct hd60s_link {
    int hdcp, dvi;
};

/* digital: MST3367 b0:0x51 bit7. r01, r34: bank 1 regs 0x01 and 0x34. DVI is only meaningful on a digital input. */
struct hd60s_link hd60s_link_from_regs(int digital, uint8_t r01, uint8_t r34);

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
    struct hd60s_settings settings;
    struct tx tx;          /* HDMI-out transmitter state */
    struct audio_settings audio;
    int audio_routed_dvi;
    struct hd60s_link link; /* refreshed by hd60s_policy_tick and hd60s_read_link */
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
/* Reads the source link flags into d->link. */
struct hd60s_link hd60s_read_link(struct hd60s *d);
/* Mode from the source's AVI InfoFrame VIC (b2:0x4B), independent of frames; NULL if none or unsupported. */
const struct tx_mode *hd60s_avi_mode(struct hd60s *d);
/* Source audio rate from the receiver's ACR packet for the given video mode; 0 if no audio or no mode. */
int hd60s_source_audio_hz(struct hd60s *d, const struct tx_mode *m);
/* Vsync period counter for hd60s_rate_from_vsync; 0 if unreadable. */
unsigned hd60s_vsync_counter(struct hd60s *d);
int i2c_read(struct hd60s *d, uint8_t addr, uint8_t sub);

#endif
