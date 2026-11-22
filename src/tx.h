#ifndef HD60S_TX_H
#define HD60S_TX_H

#include <stddef.h>
#include <stdint.h>

/* IT66121 HDMI-out transmitter at I2C 0x9A (docs/PROTOCOL.md section 6). Video only: no audio, HDCP, 0xD4 or MST3367 writes. */

enum tx_state { TX_IDLE, TX_EDID, TX_PICK, TX_UNSTABLE, TX_VIDEO, TX_ENABLE, TX_RUNNING };
enum tx_colour { TX_RGB, TX_YCC422, TX_YCC444 };

struct tx_sink {
    int valid, hdmi, ycc444, ycc422;
};

struct tx_mode {
    uint8_t vic, rep, wide;
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
};

/* NULL if the timing has no VIC. fps is the frame rate, fields counted for interlaced. */
const struct tx_mode *tx_mode_find(int width, int height, int interlaced, double fps);
int tx_edid_parse(const uint8_t *edid, size_t len, struct tx_sink *out);
void tx_avi(uint8_t out[6], enum tx_colour c, const struct tx_mode *m);
enum tx_state tx_next(enum tx_state s, struct tx_in in);
/* Only these writes ever reach the bus. */
int tx_writable(uint8_t reg, uint8_t val);
int tx_write(const struct tx_bus *b, uint8_t reg, uint8_t val);

void tx_start(struct tx *t, const struct tx_bus *b);
void tx_poll(struct tx *t, const struct tx_bus *b, const struct tx_mode *mode);
void tx_blank(struct tx *t, const struct tx_bus *b);

#endif
