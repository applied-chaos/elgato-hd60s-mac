#ifndef HD60S_TX_H
#define HD60S_TX_H

#include <stddef.h>
#include <stdint.h>

/* IT66121 HDMI-out transmitter at I2C 0x9A (docs/PROTOCOL.md section 6). Video, 2-channel PCM audio and HDCP passthrough (read-only DDC to the TV): no 0xD4 or MST3367 writes. */

enum tx_state { TX_IDLE, TX_EDID, TX_PICK, TX_UNSTABLE, TX_VIDEO, TX_ENABLE, TX_RUNNING };
enum tx_colour { TX_RGB, TX_YCC422, TX_YCC444 };
enum tx_hdcp { HDCP_OFF, HDCP_ON, HDCP_BACKOFF };
enum tx_hdcp_op { HDCP_NOP, HDCP_AUTH, HDCP_STOP };

struct tx_sink {
    int valid, hdmi, ycc444, ycc422, basic_audio;
};

struct tx_mode {
    uint8_t vic, rep, wide;
    uint32_t pclk_khz; /* TMDS clock */
};

struct tx_in {
    int hpd, stable, blank, mode_changed, edid_ok;
};

struct tx_bus {
    void *ctx;
    int (*read)(void *ctx, uint8_t reg);
    void (*write)(void *ctx, uint8_t reg, uint8_t val);
    int (*read_n)(void *ctx, uint8_t reg, uint8_t *buf, int n);
    void (*sleep_ms)(void *ctx, int ms);
};

struct tx {
    int present;
    enum tx_state state;
    struct tx_sink sink;
    enum tx_colour colour;
    const struct tx_mode *want; /* NULL = blank */
    uint8_t sent_vic;           /* 0 = nothing programmed */
    int edid_wait;
    int audio_hz;  /* programmed rate, 0 = audio off */
    int want_hz;   /* source rate from the caller, 0 = none */
    int src_hdcp;
    enum tx_hdcp hdcp;
    int hdcp_retry_polls_left;
};

/* NULL if the timing has no VIC. fps is the frame rate, fields counted for interlaced. */
const struct tx_mode *tx_mode_find(int width, int height, int interlaced, double fps);
/* NULL if the VIC is not one the transmitter can send. 16:9 SD VICs 3, 7, 18, 22 share the 4:3 timings. */
const struct tx_mode *tx_mode_from_vic(unsigned vic);
int tx_edid_parse(const uint8_t *edid, size_t len, struct tx_sink *out);
void tx_avi(uint8_t out[6], enum tx_colour c, const struct tx_mode *m);
/* N for a TMDS clock and sample rate (32, 44.1 or 48 kHz, the rates every HDMI sink must take); 0 if unsupported. */
unsigned tx_audio_n(uint32_t pclk_khz, int hz);
/* IEC 60958 channel status sampling frequency code; -1 if unsupported. */
int tx_audio_fs_code(int hz);
/* Source rate from the receiver's ACR packet (fs = pclk * N / (128 * CTS)), snapped to a standard rate; 0 if none. */
int tx_audio_rate(uint32_t pclk_khz, uint32_t n, uint32_t cts);
enum tx_state tx_next(enum tx_state s, struct tx_in in);
/* want: output running with an encrypted source and an HDMI TV. HDCP_STOP leaves HDCP_OFF; HDCP_AUTH leaves HDCP_ON or HDCP_BACKOFF by its result. */
enum tx_hdcp_op tx_hdcp_next(enum tx_hdcp s, int want, int retry_polls_left);
/* Only these writes ever reach the bus. */
int tx_writable(uint8_t reg, uint8_t val);
int tx_write(const struct tx_bus *b, uint8_t reg, uint8_t val);

void tx_start(struct tx *t, const struct tx_bus *b);
void tx_poll(struct tx *t, const struct tx_bus *b, const struct tx_mode *mode, int audio_hz, int src_hdcp);
void tx_blank(struct tx *t, const struct tx_bus *b);

#endif
