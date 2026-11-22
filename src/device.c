#include "device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ub530_tables.h"

/* Vendor requests (bmRequestType 0x40 out / 0xC0 in). Flash-writing requests are deliberately absent. */
enum {
    REQ_I2C = 0xc0,    /* wValue 0x5000|target: MCU-bridged I2C */
    REQ_GPIO = 0xc1,   /* wValue 0x4000|level<<8|pin, 0xC000|pin = input, IN: read pin */
    REQ_INIT_C2 = 0xc2,
    REQ_REPLAY = 0xc6, /* firmware I2C poll program; zero length disables it */
    REQ_INIT_C7 = 0xc7,
    REQ_INIT_EC = 0xec,
};

enum {
    I2C_MCU = 0xaa,     /* Nuvoton MCU command target */
    I2C_AUDIO = 0x94,   /* audio ADC */
    I2C_BOARD = 0x98,   /* board control */
    I2C_MST3367 = 0x9c, /* HDMI receiver, banked via register 0 */
    BRIDGE_PROXY = 0x66,
};

static void msleep(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static int ctrl(struct hd60s *d, int in, uint8_t req, uint16_t val, uint16_t idx, uint8_t *buf,
                uint16_t len)
{
    int r = libusb_control_transfer(d->h, in ? 0xc0 : 0x40, req, val, idx, buf, len, 3000);
    if (r < 0 && !d->err) {
        d->err = r;
        fprintf(stderr, "hd60s: request %02x %04x failed: %s\n", req, val, libusb_error_name(r));
    }
    return r;
}

/* MCU bridge (FUN_1c746). Targets 0x88/0x98/0x9C take the payload directly; others go via proxy 0x66. */
static int bridge_write(struct hd60s *d, uint8_t addr, const uint8_t *w, int wl)
{
    uint8_t b[72];
    if (wl + 1 > (int)sizeof b)
        return -1;
    if (addr == 0x88 || addr == I2C_BOARD || addr == I2C_MST3367) {
        memcpy(b, w, (size_t)wl);
        return ctrl(d, 0, REQ_I2C, 0x5000 | addr, 0, b, (uint16_t)wl);
    }
    b[0] = addr;
    memcpy(b + 1, w, (size_t)wl);
    return ctrl(d, 0, REQ_I2C, 0x5000 | BRIDGE_PROXY, 0, b, (uint16_t)(wl + 1));
}

static int bridge_read(struct hd60s *d, uint8_t addr, const uint8_t *w, int wl, uint8_t *r, int rl)
{
    uint8_t b[72];
    if (wl + 2 > (int)sizeof b)
        return -1;
    b[0] = addr | 1;
    b[1] = (uint8_t)rl;
    memcpy(b + 2, w, (size_t)wl);
    if (ctrl(d, 0, REQ_I2C, 0x5000 | BRIDGE_PROXY, 0, b, (uint16_t)(wl + 2)) < 0)
        return -1;
    return ctrl(d, 1, REQ_I2C, 0x5000 | BRIDGE_PROXY, 0, r, (uint16_t)rl);
}

static void i2c_write(struct hd60s *d, uint8_t addr, uint8_t sub, uint8_t val)
{
    bridge_write(d, addr, (uint8_t[]){sub, val}, 2);
}

/* Returns 0xFF on failure, matching Elgato's register helpers. */
int i2c_read(struct hd60s *d, uint8_t addr, uint8_t sub)
{
    uint8_t v = 0;
    return bridge_read(d, addr, &sub, 1, &v, 1) < 0 ? 0xff : v;
}

static void mst_write(struct hd60s *d, uint8_t bank, uint8_t reg, uint8_t val)
{
    i2c_write(d, I2C_MST3367, 0, bank);
    i2c_write(d, I2C_MST3367, reg, val);
}

int mst_read(struct hd60s *d, uint8_t bank, uint8_t reg)
{
    i2c_write(d, I2C_MST3367, 0, bank);
    return i2c_read(d, I2C_MST3367, reg);
}

static void mst_update(struct hd60s *d, uint8_t bank, uint8_t reg, uint8_t keep, uint8_t set)
{
    mst_write(d, bank, reg, (uint8_t)((mst_read(d, bank, reg) & keep) | set));
}

static void board_update(struct hd60s *d, uint8_t reg, uint8_t keep, uint8_t set)
{
    i2c_write(d, I2C_BOARD, reg, (uint8_t)((i2c_read(d, I2C_BOARD, reg) & keep) | set));
}

/* Transmitter bus: IT66121 at 0x9A only. */
enum { I2C_TX = 0x9a };

static int tx_bus_read(void *c, uint8_t reg) { return i2c_read(c, I2C_TX, reg); }
static void tx_bus_write(void *c, uint8_t reg, uint8_t val) { i2c_write(c, I2C_TX, reg, val); }
static int tx_bus_read_n(void *c, uint8_t reg, uint8_t *buf, int n) { return bridge_read(c, I2C_TX, &reg, 1, buf, n); }
static void tx_bus_sleep(void *c, int ms) { (void)c; msleep(ms); }

static struct tx_bus tx_bus(struct hd60s *d)
{
    return (struct tx_bus){d, tx_bus_read, tx_bus_write, tx_bus_read_n, tx_bus_sleep};
}

static int gpio_read(struct hd60s *d, uint8_t pin)
{
    uint8_t v = 0;
    return ctrl(d, 1, REQ_GPIO, pin, 0, &v, 1) < 0 ? 0xff : v;
}

/* FUN_1c910: enable the MCU and HDMI receiver, then find the MCU bridge. */
static int power_up(struct hd60s *d)
{
    ctrl(d, 0, REQ_GPIO, 0xc039, 0, NULL, 0); /* GPIO 57 input: MCU ready */
    ctrl(d, 0, REQ_GPIO, 0x4134, 0, NULL, 0); /* GPIO 52 high: MCU/receiver enable */
    for (int i = 0; i < 100; i++) {
        msleep(10);
        if (gpio_read(d, 0x39) == 1)
            break;
    }
    for (int i = 0; i < 200; i++) {
        uint8_t r[3] = {0};
        bridge_read(d, I2C_MCU, (uint8_t[]){0x12, 0x34, 0x57}, 3, r, 3);
        if (r[0] == 'Q' && r[1] == 0x10) {
            bridge_read(d, I2C_MCU, (uint8_t[]){0x12, 0x34, 0x58}, 3, r, 3);
            d->mcu_version = (uint32_t)r[0] << 16 | (uint32_t)r[1] << 8 | r[2];
            mst_write(d, 0, 0x07, 0xa5);
            return mst_read(d, 0, 0x07) == 0xa5 ? 0 : -1;
        }
        if (r[0] != '3' || r[1] != 'D' || r[2] != 0x55)
            break;
        msleep(100);
    }
    fprintf(stderr, "hd60s: MCU bridge did not answer\n");
    return -1;
}

static void power_down(struct hd60s *d)
{
    ctrl(d, 0, REQ_GPIO, 0xc039, 0, NULL, 0);
    ctrl(d, 0, REQ_GPIO, 0x4034, 0, NULL, 0);
}

static void replay_disable(struct hd60s *d) { ctrl(d, 0, REQ_REPLAY, 0, 0x100, NULL, 0); }

/* FUN_11997 with the HD60 S branch only. */
static void mst_route(struct hd60s *d, unsigned mode)
{
    mst_update(d, 0, 0xb7, 0xfd, (mode & 2) ? 0 : 2);
}

/* FUN_a2a6 for the HD60 S: board control defaults. */
static void board_defaults(struct hd60s *d)
{
    uint8_t v = 0;
    if (bridge_read(d, I2C_BOARD, (uint8_t[]){0x3b}, 1, &v, 1) < 0 || v != 0)
        i2c_write(d, I2C_BOARD, 0x3b, 0x02);
    i2c_write(d, I2C_BOARD, 0x20, 0x05);
    i2c_write(d, I2C_BOARD, 0x00, 0x00);
    i2c_write(d, I2C_BOARD, 0x10, 0xfe);
}

/* FUN_10e8a for internal input 0 (HDMI) with HDMI audio. */
static void select_hdmi(struct hd60s *d)
{
    board_defaults(d);
    mst_route(d, 0);
    mst_write(d, 0, 0x41, 0x6f);
    mst_write(d, 0, 0xb8, 0x00);
    mst_write(d, 1, 0x0f, 0x02);
    mst_write(d, 1, 0x16, 0x30);
    mst_write(d, 1, 0x17, 0x02);
    mst_write(d, 1, 0x18, 0x02);
    mst_write(d, 1, 0x19, 0x02);
    mst_write(d, 1, 0x1a, 0x50);
    mst_update(d, 1, 0x2a, 0xff, 0x07);
    mst_write(d, 2, 0x08, 0x03);

    /* FUN_10df5: snapshot bank 1 reg 0x27 (read five times) for the stop sequence. */
    mst_write(d, 1, 0x25, 0);
    mst_write(d, 1, 0x26, 0);
    for (int i = 0; i < 5; i++) {
        msleep(20);
        d->mst_saved[i] = (uint8_t)mst_read(d, 1, 0x27);
    }

    mst_write(d, 1, 0x24, 0x40);
    if (mst_read(d, 1, 0x24) & 1) {
        mst_write(d, 1, 0x25, 0);
        mst_write(d, 1, 0x26, 0);
        for (int i = 0; i < 5; i++)
            mst_write(d, 1, 0x27, 0);
    }
    mst_write(d, 1, 0x30, 0x80);
    mst_write(d, 1, 0x31, 0x00);
    mst_write(d, 1, 0x32, 0x00);
    mst_write(d, 0, 0xb0, 0x14);
    mst_update(d, 0, 0xae, 0xff, 0x04);
    mst_write(d, 0, 0xad, 0x05);
    mst_write(d, 0, 0xb1, 0xc0);
    mst_write(d, 0, 0xb2, 0x00);
    mst_write(d, 0, 0xb3, 0x00);
    mst_write(d, 0, 0xb4, 0x55);
    mst_update(d, 0, 0xb4, 0xfc, 0);
    mst_write(d, 2, 0x01, 0x61);
    mst_write(d, 2, 0x02, 0xf5);
    mst_update(d, 2, 0x03, 0xff, 0x02);
    mst_write(d, 2, 0x04, 0x01);
    mst_write(d, 2, 0x05, 0x00);
    mst_write(d, 2, 0x06, 0x08);
    mst_write(d, 2, 0x1c, 0x1a);
    mst_write(d, 2, 0x1d, 0x00);
    mst_write(d, 2, 0x1e, 0x00);
    mst_write(d, 2, 0x1f, 0x00);
    mst_update(d, 2, 0x25, 0xff, 0xa2);
    mst_update(d, 2, 0x02, 0xff, 0x80);
    mst_update(d, 2, 0x07, 0xff, 0x04);
    mst_write(d, 2, 0x17, 0xc0);
    mst_write(d, 2, 0x19, 0xff);
    mst_write(d, 2, 0x1a, 0xff);
    mst_write(d, 2, 0x1b, 0xfc);
    mst_write(d, 2, 0x20, 0x00);
    mst_update(d, 2, 0x21, 0xfc, 0);
    mst_write(d, 2, 0x22, 0x26);
    mst_write(d, 2, 0x27, 0xff); /* HDMI audio */
    mst_update(d, 2, 0x2e, 0xff, 0xa1);

    mst_route(d, 0);
    mst_write(d, 0, 0xb8, 0x10);
    mst_write(d, 0, 0xb8, 0x00);
    mst_write(d, 2, 0x07, 0xf4);
    mst_write(d, 2, 0x07, 0x04);
    mst_write(d, 0, 0x51, 0x89);
    mst_route(d, 3);
    board_update(d, 0x20, 0xff, 0x10);
    mst_write(d, 0, 0xb7, 0x00);

    /* FUN_11b3f */
    mst_update(d, 2, 0x01, 0x0f, 0x60);
    mst_update(d, 2, 0x04, 0xff, 0x01);
    mst_write(d, 2, 0x06, 0x08);
    mst_update(d, 2, 0x09, 0xff, 0x20);
    mst_update(d, 0, 0x54, 0xef, 0);
    mst_update(d, 0, 0xac, 0xff, 0x80);
    mst_update(d, 0, 0x00, 0xff, 0x80);
    mst_update(d, 0, 0xce, 0xff, 0x80);
    mst_update(d, 0, 0xcf, 0xf8, 0x02);
    mst_update(d, 0, 0x00, 0x7f, 0);
    uint8_t t = ub530_tx_phase[0];
    uint8_t d0 = (uint8_t)mst_read(d, 0x80, 0xd0), cf = (uint8_t)mst_read(d, 0x80, 0xcf);
    mst_write(d, 0x80, 0xd0, (uint8_t)((t >> 1 & 3) | (d0 & 0xfc)));
    mst_write(d, 0x80, 0xcf, (uint8_t)((t << 7) | (cf & 0x7f)));
    msleep(100);
}

/* FUN_1caf4 with audio source 0 (HDMI). */
static void select_hdmi_audio(struct hd60s *d)
{
    int dvi = 0;
    if (mst_read(d, 0, 0x51) & 0x80)
        dvi = mst_read(d, 1, 0x01) >> 2 & 1;
    i2c_write(d, I2C_AUDIO, 0x09, 0x46);
    uint8_t r4 = (uint8_t)i2c_read(d, I2C_AUDIO, 0x04);
    if (!dvi) {
        mst_write(d, 2, 0x27, 0xff);
        r4 = (r4 & 0xbc) | 0x40;
        i2c_write(d, I2C_AUDIO, 0x03, 0xa1);
        bridge_write(d, I2C_MCU, (uint8_t[]){0x12, 0x34, 0x90, 0x05, 0x01}, 5);
        bridge_write(d, I2C_MCU, (uint8_t[]){0x12, 0x34, 0x90, 0x03, 0x01}, 5);
    } else {
        mst_write(d, 2, 0x27, 0x00);
        r4 = (r4 & 0xbc) | 0x02;
        i2c_write(d, I2C_AUDIO, 0x03, 0xa0);
        bridge_write(d, I2C_MCU, (uint8_t[]){0x12, 0x34, 0x90, 0x05, 0x00}, 5);
        bridge_write(d, I2C_MCU, (uint8_t[]){0x12, 0x34, 0x90, 0x03, 0x00}, 5);
    }
    /* Elgato source 0 = HDMI only: mute the analogue line-in pair (0x0e/0x0f), unmute HDMI (0x10/0x11). */
    uint8_t le = (uint8_t)(i2c_read(d, I2C_AUDIO, 0x0e) | 0x80);
    uint8_t ri = (uint8_t)(i2c_read(d, I2C_AUDIO, 0x10) & 0x7f);
    i2c_write(d, I2C_AUDIO, 0x0e, le);
    i2c_write(d, I2C_AUDIO, 0x0f, le);
    i2c_write(d, I2C_AUDIO, 0x10, ri);
    i2c_write(d, I2C_AUDIO, 0x11, ri);
    i2c_write(d, I2C_AUDIO, 0x04, r4);
    i2c_write(d, I2C_AUDIO, 0x07, 0x00);
}

/* FUN_103ea's clamp: coefficients are 15-bit with bit 14 as sign. */
static uint16_t coeff_clamp(uint16_t c)
{
    uint16_t v = c & 0x7fff;
    if (v & 0x4000)
        return v <= 0x4000 ? 0x4000 : v;
    return v < 0x3fff ? v : 0x3fff;
}

/* FUN_10b6b + FUN_103ea: RGB to YCbCr matrix; BT.601 for SD, BT.709 otherwise. */
static void colour_convert(struct hd60s *d, int sd, int input_is_yuv)
{
    uint8_t raw[24];
    uint8_t mode = 0x62;
    if (input_is_yuv) {
        memcpy(raw, ub530_csc_rgb, 16);
        memcpy(raw + 16, ub530_csc_rgb_tail, 8);
    } else {
        memcpy(raw, sd ? ub530_csc_601 : ub530_csc_709, 16);
        memcpy(raw + 16, sd ? ub530_csc_601_tail : ub530_csc_709_tail, 2);
        memcpy(raw + 18, ub530_csc_rgb_tail + 2, 6);
        mode = 0x40;
    }
    uint16_t m[12];
    for (int i = 0; i < 12; i++)
        m[i] = (uint16_t)(raw[2 * i] | raw[2 * i + 1] << 8);

    uint8_t ab = (uint8_t)mst_read(d, 0, 0xab);
    mst_write(d, 0, 0xab, ab | 0x80);
    mst_write(d, 0, 0x90, 0x15);
    mst_write(d, 0, 0x91, 0x15);
    mst_write(d, 0, 0x92, mode);

    /* Picture controls at Elgato's defaults (brightness 0x81, others neutral). */
    // ponytail: brightness/contrast/saturation/hue fixed; port FUN_103ea's maths if controls are wanted
    for (int i = 0; i < 9; i++)
        m[i] = coeff_clamp(m[i]);
    m[10] = (uint16_t)(((0x40 + m[10]) & 0x7fc0) | 1);
    mst_update(d, 0, 0xab, 0x80, 0x15);
    mst_update(d, 0, 0xac, 0xc0, 0x15);
    mst_write(d, 0, 0xad, 0x05);
    mst_write(d, 0, 0x1e, 0x11);
    mst_write(d, 0, 0x1f, 0x01);
    for (int i = 0; i < 12; i++) {
        mst_write(d, 0, (uint8_t)(0x93 + 2 * i), (uint8_t)(m[i] >> 8));
        mst_write(d, 0, (uint8_t)(0x94 + 2 * i), (uint8_t)m[i]);
    }
    mst_update(d, 0, 0xb0, 0xc2, 0x21);
    mst_write(d, 0, 0xab, ab & 0x7f);
}

/* Mirrors FUN_cc7e / FUN_cf6c: pulse bank 1 reg 0x24 with outputs muted. */
static void receiver_pulse(struct hd60s *d, uint8_t r24, const uint8_t *r27)
{
    uint8_t ab = (uint8_t)mst_read(d, 0, 0xab);
    uint8_t a27 = (uint8_t)mst_read(d, 2, 0x27);
    mst_write(d, 0, 0xab, ab | 0x80);
    mst_write(d, 2, 0x27, 0xff);
    mst_write(d, 0, 0xb3, 0xff);
    mst_write(d, 1, 0x24, r24);
    mst_write(d, 1, 0x25, 0);
    mst_write(d, 1, 0x26, 0);
    for (int i = 0; i < 5; i++)
        mst_write(d, 1, 0x27, r27 ? r27[i] : 0);
    mst_write(d, 0, 0xab, ab & 0x7f);
    mst_write(d, 2, 0x27, a27);
    mst_write(d, 0, 0xb3, 0);
}

int hd60s_quiet;

int hd60s_open(struct hd60s *d)
{
    memset(d, 0, sizeof *d);
    if (libusb_init(&d->usb) < 0)
        return -1;
    d->open_err = LIBUSB_ERROR_NO_DEVICE;
    libusb_device **list;
    ssize_t n = libusb_get_device_list(d->usb, &list);
    for (ssize_t i = 0; i < n && !d->h; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) == 0 && dd.idVendor == HD60S_VID && dd.idProduct == HD60S_PID)
            d->open_err = libusb_open(list[i], &d->h);
    }
    if (n >= 0)
        libusb_free_device_list(list, 1);
    if (!d->h) {
        if (!hd60s_quiet)
            fprintf(stderr, "hd60s: no %04x:%04x found (or it is busy)\n", HD60S_VID, HD60S_PID);
        return -1;
    }
    int cfg = 0;
    libusb_get_configuration(d->h, &cfg);
    if (cfg != 1 && (d->open_err = libusb_set_configuration(d->h, 1)) < 0)
        return -1;
    if ((d->open_err = libusb_claim_interface(d->h, 0)) < 0) {
        if (!hd60s_quiet)
            fprintf(stderr, "hd60s: cannot claim interface 0\n");
        return -1;
    }

    /* FUN_d118 */
    uint8_t ec[10] = {0xb8, 0x22, 0, 0, 0, 0xc0, 0, 0, 0, 0xca};
    /* Elgato ignores these results; 0xC2 stalls on every other open. The bridge probe is the real check. */
    libusb_control_transfer(d->h, 0x40, REQ_INIT_EC, 0, 0, ec, sizeof ec, 3000);
    libusb_control_transfer(d->h, 0x40, REQ_INIT_C2, 0, 0, NULL, 0, 3000);
    libusb_control_transfer(d->h, 0x40, REQ_INIT_C7, 100, 0, NULL, 0, 3000);
    if (power_up(d) < 0)
        return -1;
    replay_disable(d);
    return d->err ? -1 : 0;
}

int hd60s_start(struct hd60s *d)
{
    select_hdmi(d);
    select_hdmi_audio(d);
    i2c_write(d, I2C_BOARD, 0x20, 0x05);

    receiver_pulse(d, 0x41, NULL);
    for (int i = 0; i < 10 && !(mst_read(d, 2, 0x0e) & 7); i++)
        msleep(5);
    for (int i = 0; i < 500; i++) {
        uint8_t v = (uint8_t)mst_read(d, 2, 0x0e);
        mst_write(d, 2, 0x0e, v);
        if (!(v & 7))
            break;
        msleep(5);
    }
    colour_convert(d, 0, 0);
    d->sd = 0;
    struct tx_bus tb = tx_bus(d);
    tx_start(&d->tx, &tb);
    d->started = !d->err;
    return d->err ? -1 : 0;
}

void hd60s_stop(struct hd60s *d)
{
    if (!d->h)
        return;
    if (d->started) {
        struct tx_bus tb = tx_bus(d);
        tx_blank(&d->tx, &tb);
        receiver_pulse(d, 0x40, d->mst_saved);
        d->started = 0;
    }
    power_down(d);
    replay_disable(d);
}

void hd60s_close(struct hd60s *d)
{
    if (d->h) {
        libusb_release_interface(d->h, 0);
        libusb_close(d->h);
    }
    if (d->usb)
        libusb_exit(d->usb);
    memset(d, 0, sizeof *d);
}

#define NUM_XFERS 16
#define XFER_SIZE (512 * 1024)

#define TX_POLL_MS 250
#define MODE_WINDOW_MS 2000

/* Frame size and rate over a window. The size must hold for the last 3/4 of the window's frames (startup runts and mode switches fail this). */
struct stream_ctx {
    struct hd60s_parser *parser;
    volatile sig_atomic_t *stop;
    int active;
    frame_cb frame_cb;
    void *frame_ctx;
    int w, h, interlaced, frames, run;
};

static void on_frame_probe(void *ctx, const uint8_t *f, int w, int h, int interlaced)
{
    struct stream_ctx *s = ctx;
    s->run = s->frames && w == s->w && h == s->h && interlaced == s->interlaced ? s->run + 1 : 1;
    s->w = w;
    s->h = h;
    s->interlaced = interlaced;
    s->frames++;
    s->frame_cb(s->frame_ctx, f, w, h, interlaced);
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void LIBUSB_CALL on_xfer(struct libusb_transfer *t)
{
    struct stream_ctx *s = t->user_data;
    if (t->status == LIBUSB_TRANSFER_COMPLETED || t->status == LIBUSB_TRANSFER_TIMED_OUT) {
        parser_feed(s->parser, t->buffer, (size_t)t->actual_length);
    } else if (t->status != LIBUSB_TRANSFER_CANCELLED) {
        fprintf(stderr, "hd60s: transfer status %d\n", t->status);
        *s->stop = 1;
    }
    if (!*s->stop && libusb_submit_transfer(t) == 0)
        return;
    s->active--;
}

/* Source AVI InfoFrame (same MST3367 bank 2 block the driver reads the VIC from): DB2 bits 5:4 = 2 is 16:9. */
static int source_is_16x9(struct hd60s *d)
{
    return (mst_read(d, 2, 0x0b) & 0x08) && ((mst_read(d, 2, 0x49) >> 4) & 3) == 2;
}

int hd60s_stream(struct hd60s *d, struct hd60s_parser *p, volatile sig_atomic_t *stop, double seconds)
{
    struct stream_ctx s = {.parser = p, .stop = stop, .frame_cb = p->on_frame, .frame_ctx = p->ctx};
    struct libusb_transfer *xfers[NUM_XFERS];
    if (libusb_set_interface_alt_setting(d->h, 0, 4) < 0) {
        fprintf(stderr, "hd60s: cannot select bulk alt setting\n");
        return -1;
    }
    p->on_frame = on_frame_probe;
    p->ctx = &s;
    const struct tx_mode *mode = NULL;
    struct tx_bus tb = tx_bus(d);
    long win_start = now_ms(), last_poll = win_start;
    for (int i = 0; i < NUM_XFERS; i++) {
        xfers[i] = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(xfers[i], d->h, HD60S_EP_VIDEO, malloc(XFER_SIZE), XFER_SIZE,
                                  on_xfer, &s, 1000);
        if (libusb_submit_transfer(xfers[i]) == 0)
            s.active++;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    time_t end = ts.tv_sec + (time_t)seconds;
    time_t aspect_at = 0;
    while (s.active) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if (ts.tv_sec != aspect_at) {
            aspect_at = ts.tv_sec;
            p->wide = source_is_16x9(d);
        }
        if (*stop || (seconds > 0 && ts.tv_sec >= end)) {
            *stop = 1;
            for (int i = 0; i < NUM_XFERS; i++)
                libusb_cancel_transfer(xfers[i]);
        }
        struct timeval tv = {0, 100000};
        libusb_handle_events_timeout_completed(d->usb, &tv, NULL);
        /* Not from the transfer callback: the sync control transfers need the event loop. */
        int sd = parser_is_sd(p->last_w, p->last_h);
        if (sd >= 0 && sd != d->sd) {
            colour_convert(d, sd, 0);
            d->sd = sd;
        }
        long now = now_ms();
        if (now - win_start >= MODE_WINDOW_MS) {
            double fps = s.frames * (s.interlaced ? 2000.0 : 1000.0) / (double)(now - win_start);
            mode = s.frames && s.run * 4 >= s.frames * 3 ? tx_mode_find(s.w, s.h, s.interlaced, fps) : NULL;
            s.frames = 0;
            win_start = now;
        }
        if (!*stop && now - last_poll >= TX_POLL_MS) {
            tx_poll(&d->tx, &tb, mode);
            last_poll = now;
        }
    }
    p->on_frame = s.frame_cb;
    p->ctx = s.frame_ctx;
    for (int i = 0; i < NUM_XFERS; i++) {
        free(xfers[i]->buffer);
        libusb_free_transfer(xfers[i]);
    }
    libusb_set_interface_alt_setting(d->h, 0, 0);
    return 0;
}
