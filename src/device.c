#include "device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void msleep(struct hd60s *d, int ms) { d->usb.sleep_ms(d->usb.ctx, ms); }

static int ctrl(struct hd60s *d, int in, uint8_t req, uint16_t val, uint16_t idx, uint8_t *buf,
                uint16_t len)
{
    int r = d->usb.ctrl(d->usb.ctx, in, req, val, idx, buf, len);
    if (r < 0 && !d->err)
        d->err = r;
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

unsigned hd60s_vsync_counter(struct hd60s *d)
{
    int hi = mst_read(d, 0, 0x59), lo = mst_read(d, 0, 0x5a);
    return hi == 0xff && lo == 0xff ? 0 : (unsigned)(hi << 8 | lo) & 0x3fff;
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
static void tx_bus_sleep(void *c, int ms) { msleep(c, ms); }

static struct tx_bus tx_bus(struct hd60s *d)
{
    return (struct tx_bus){d, tx_bus_read, tx_bus_write, tx_bus_read_n, tx_bus_sleep};
}

/* Source sample rate from the receiver's ACR packet (b2:0x40-42 CTS, 0x43-45 N). 0 when b2:0x0b shows no audio. */
int hd60s_source_audio_hz(struct hd60s *d, const struct tx_mode *m)
{
    if (!m || (mst_read(d, 2, 0x0b) & 0x62) != 0x62)
        return 0;
    uint32_t v[6];
    for (int i = 0; i < 6; i++)
        v[i] = (uint32_t)mst_read(d, 2, (uint8_t)(0x40 + i));
    uint32_t cts = v[0] | v[1] << 8 | (v[2] & 0xf) << 16, n = v[3] | v[4] << 8 | (v[5] & 0xf) << 16;
    return tx_audio_rate(m->pclk_khz, n, cts);
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
        msleep(d, 10);
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
        msleep(d, 100);
    }
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
        msleep(d, 20);
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
    msleep(d, 100);
}

static int source_is_dvi(struct hd60s *d)
{
    return (mst_read(d, 0, 0x51) & 0x80) ? mst_read(d, 1, 0x01) >> 2 & 1 : 0;
}

/* FUN_1caf4 (routing) and FUN_1ce18 (gain) for d->audio. */
static void apply_audio(struct hd60s *d)
{
    int dvi = source_is_dvi(d);
    struct audio_regs cur = {.r04 = (uint8_t)i2c_read(d, I2C_AUDIO, 0x04)}, r;
    for (int i = 0; i < 4; i++) {
        cur.gain[i] = (uint8_t)i2c_read(d, I2C_AUDIO, (uint8_t)(0x0a + i));
        cur.pair[i] = (uint8_t)i2c_read(d, I2C_AUDIO, (uint8_t)(0x0e + i));
    }
    audio_plan(&d->audio, dvi, &cur, &r);
    i2c_write(d, I2C_AUDIO, 0x09, 0x46);
    mst_write(d, 2, 0x27, dvi ? 0x00 : 0xff);
    i2c_write(d, I2C_AUDIO, 0x03, r.r03);
    uint8_t mcu = dvi ? 0 : 1;
    bridge_write(d, I2C_MCU, (uint8_t[]){0x12, 0x34, 0x90, 0x05, mcu}, 5);
    bridge_write(d, I2C_MCU, (uint8_t[]){0x12, 0x34, 0x90, 0x03, mcu}, 5);
    for (int i = 0; i < 4; i++) {
        i2c_write(d, I2C_AUDIO, (uint8_t)(0x0e + i), r.pair[i]);
        i2c_write(d, I2C_AUDIO, (uint8_t)(0x0a + i), r.gain[i]);
    }
    i2c_write(d, I2C_AUDIO, 0x04, r.r04);
    i2c_write(d, I2C_AUDIO, 0x07, r.r07);
    d->audio_routed_dvi = dvi;
}

int hd60s_set_audio(struct hd60s *d, const struct audio_settings *s)
{
    d->audio = *s;
    d->audio.gain_half_db = audio_gain_clamp(s->gain_half_db);
    if (d->started)
        apply_audio(d);
    return d->err ? -1 : 0;
}

/* FUN_103ea's clamp: words are 15-bit with bit 14 as sign, and the sign comes from the word before scaling. */
static uint16_t coeff_clamp(uint16_t src, uint16_t v)
{
    if (src & 0x4000)
        return v <= 0x4000 ? 0x4000 : v;
    return v < 0x3fff ? v : 0x3fff;
}

/* FUN_103ea works on a word doubled into int16, scaled by x/10000 (truncating), then halved back to 15 bits. */
static int wide(uint16_t w)
{
    return (int16_t)(w * 2);
}

static uint16_t narrow(int x)
{
    return (uint16_t)((uint16_t)(x / 10000) >> 1);
}

static uint16_t scale_word(uint16_t w, int k)
{
    return coeff_clamp(w, narrow(wide(w) * k));
}

static int hue_table(const uint8_t *t, int i)
{
    return t[2 * i] | t[2 * i + 1] << 8;
}

/* hue_table(t, i) interpolated a quarter step at a time, truncating towards zero. */
static int hue_lerp(const uint8_t *t, int i, int quarter)
{
    return (int16_t)((hue_table(t, i + 1) - hue_table(t, i)) * quarter / 4 + hue_table(t, i));
}

static int pic_byte(int base, int8_t delta)
{
    int v = base + delta;
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* FUN_103ea: brightness is an offset, contrast scales the luma words, saturation the chroma words, hue rotates them. */
static void apply_picture(uint16_t m[12], const struct hd60s_picture *p)
{
    int bri = pic_byte(0x81, p->brightness), con = pic_byte(0x80, p->contrast);
    int sat = pic_byte(0x80, p->saturation), hue = pic_byte(0x80, p->hue);
    if (sat == 0)
        sat = 1;
    int t = bri * 5 - 0x280;
    m[10] = (uint16_t)((((t + (t < 0 ? 3 : 0)) * 16 & 0xffc0) + m[10]) & 0x7fc0) | 1;

    static const int luma[3] = {1, 4, 7}, chroma[6] = {0, 3, 6, 2, 5, 8};
    for (int i = 0; i < 3; i++)
        m[luma[i]] = scale_word(m[luma[i]], con * 10000 >> 7);
    for (int i = 0; i < 6; i++)
        m[chroma[i]] = scale_word(m[chroma[i]], sat * 10000 >> 7);

    uint8_t a = hue == 0 ? 0xff : (uint8_t)-hue;
    int negative = a >= 0x80, step = negative ? a ^ 0x80 : 0x80 - a;
    int s = hue_lerp(ub530_hue_sin, step >> 2, step & 3), c = hue_lerp(ub530_hue_cos, step >> 2, step & 3);
    if (!negative)
        s = -s;
    for (int i = 0; i < 3; i++) {
        int x = wide(m[chroma[i]]), y = wide(m[chroma[i + 3]]);
        m[chroma[i]] = narrow(x * c - y * s);
        m[chroma[i + 3]] = narrow(x * s + y * c);
    }
}

/* AVI InfoFrame bytes as the MST3367 holds them. Elgato treats the reserved Y1Y0 = 3 as RGB. */
struct hd60s_source hd60s_parse_avi(int present, uint8_t db1, uint8_t db2, uint8_t db3)
{
    struct hd60s_source s = {INPUT_RGB, COLOUR_UNKNOWN, 0};
    if (!present)
        return s;
    int y = db1 >> 5 & 3, c = db2 >> 6;
    s.format = y == 1 ? INPUT_YCBCR422 : y == 2 ? INPUT_YCBCR444 : INPUT_RGB;
    s.colorimetry = c == 1 ? COLOUR_BT601 : c == 2 ? COLOUR_BT709 : COLOUR_UNKNOWN;
    s.quant = db3 >> 2 & 3;
    return s;
}

/* Elgato's dev+0x7c: one nibble per source Q (Q=0 bits 0-3, Q=1 bits 4-7, Q=2 bits 8-11). Bit 0 compresses, bit 1 expands. */
static const uint32_t range_word[] = {[RANGE_AUTO] = 0x80000100, [RANGE_STANDARD] = 0x80000000, [RANGE_EXPANDED] = 0x80000111};

static unsigned range_bits(enum hd60s_range r, int quant)
{
    return range_word[r] >> (quant == 2 ? 8 : quant == 1 ? 4 : 0) & 0xf;
}

/* FUN_10b6b + FUN_103ea: RGB input gets an RGB to YCbCr matrix (601 for SD, else 709); YCbCr input passes through. */
void hd60s_csc_plan(const struct hd60s_colour_cfg *c, struct hd60s_csc *out)
{
    uint8_t raw[24];
    memset(out, 0, sizeof *out);
    if (c->input != INPUT_RGB) {
        memcpy(raw, ub530_csc_rgb, 16);
        memcpy(raw + 16, ub530_csc_rgb_tail, 8);
        out->mode = 0x62;
    } else {
        memcpy(raw, c->sd ? ub530_csc_601 : ub530_csc_709, 16);
        memcpy(raw + 16, c->sd ? ub530_csc_601_tail : ub530_csc_709_tail, 2);
        memcpy(raw + 18, ub530_csc_rgb_tail + 2, 6);
        out->mode = 0x40;
    }
    for (int i = 0; i < 12; i++)
        out->m[i] = (uint16_t)(raw[2 * i] | raw[2 * i + 1] << 8);
    unsigned bits = range_bits(c->range, c->quant);
    uint32_t k = 0x1000;
    if (bits & 2) {
        k = 0x129e;
        out->mode |= out->mode & 0x22 ? 4 : 0x15;
    } else if (bits & 1) {
        k = 0xdc0;
        out->m[10] = 0x400;
    }
    for (int i = 0; i < 9; i++)
        out->m[i] = (uint16_t)((uint32_t)((int16_t)out->m[i] * (int32_t)k) >> 12);
    apply_picture(out->m, &c->picture);
}

enum hd60s_colour hd60s_frame_colour(const struct hd60s_colour_cfg *c, enum hd60s_colour colorimetry)
{
    if (c->input != INPUT_RGB && colorimetry != COLOUR_UNKNOWN)
        return colorimetry;
    return c->sd ? COLOUR_BT601 : COLOUR_BT709;
}

static void colour_convert(struct hd60s *d, const struct hd60s_colour_cfg *c)
{
    struct hd60s_csc csc;
    hd60s_csc_plan(c, &csc);
    uint8_t ab = (uint8_t)mst_read(d, 0, 0xab);
    mst_write(d, 0, 0xab, ab | 0x80);
    mst_write(d, 0, 0x90, 0x15);
    mst_write(d, 0, 0x91, 0x15);
    mst_write(d, 0, 0x92, csc.mode);
    mst_update(d, 0, 0xab, 0x80, 0x15);
    mst_update(d, 0, 0xac, 0xc0, 0x15);
    mst_write(d, 0, 0xad, 0x05);
    mst_write(d, 0, 0x1e, 0x11);
    mst_write(d, 0, 0x1f, 0x01);
    for (int i = 0; i < 12; i++) {
        mst_write(d, 0, (uint8_t)(0x93 + 2 * i), (uint8_t)(csc.m[i] >> 8));
        mst_write(d, 0, (uint8_t)(0x94 + 2 * i), (uint8_t)csc.m[i]);
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

int hd60s_init(struct hd60s *d)
{
    /* FUN_d118. Elgato ignores these results; 0xC2 stalls on every other open. The bridge probe is the real check. */
    uint8_t ec[10] = {0xb8, 0x22, 0, 0, 0, 0xc0, 0, 0, 0, 0xca};
    d->usb.ctrl(d->usb.ctx, 0, REQ_INIT_EC, 0, 0, ec, sizeof ec);
    d->usb.ctrl(d->usb.ctx, 0, REQ_INIT_C2, 0, 0, NULL, 0);
    d->usb.ctrl(d->usb.ctx, 0, REQ_INIT_C7, 100, 0, NULL, 0);
    if (power_up(d) < 0)
        return -1;
    replay_disable(d);
    d->audio = AUDIO_DEFAULT;
    return d->err ? -1 : 0;
}

int hd60s_start(struct hd60s *d)
{
    select_hdmi(d);
    apply_audio(d);
    i2c_write(d, I2C_BOARD, 0x20, 0x05);

    receiver_pulse(d, 0x41, NULL);
    for (int i = 0; i < 10 && !(mst_read(d, 2, 0x0e) & 7); i++)
        msleep(d, 5);
    for (int i = 0; i < 500; i++) {
        uint8_t v = (uint8_t)mst_read(d, 2, 0x0e);
        mst_write(d, 2, 0x0e, v);
        if (!(v & 7))
            break;
        msleep(d, 5);
    }
    d->colour = (struct hd60s_colour_cfg){INPUT_RGB, 0, d->settings.range, 0, d->settings.picture};
    colour_convert(d, &d->colour);
    struct tx_bus tb = tx_bus(d);
    tx_start(&d->tx, &tb);
    d->started = !d->err;
    return d->err ? -1 : 0;
}

void hd60s_stop(struct hd60s *d)
{
    if (!d->usb.ctrl)
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

enum { TX_POLL_MS = 250, MODE_WINDOW_MS = 2000, ASPECT_POLL_MS = 1000 };

void hd60s_policy_begin(struct hd60s_policy *s, long now_ms)
{
    memset(s, 0, sizeof *s);
    s->win_start = s->last_poll = now_ms;
    s->aspect_at = now_ms - ASPECT_POLL_MS;
    s->src.format = INPUT_RGB;
}

void hd60s_policy_frame(struct hd60s_policy *s, int w, int h, int interlaced)
{
    s->run = s->frames && w == s->w && h == s->h && interlaced == s->interlaced ? s->run + 1 : 1;
    s->w = w;
    s->h = h;
    s->interlaced = interlaced;
    s->frames++;
}

void hd60s_policy_tick(struct hd60s *d, struct hd60s_parser *p, struct hd60s_policy *s, long now_ms)
{
    if (now_ms - s->aspect_at >= ASPECT_POLL_MS) {
        s->aspect_at = now_ms;
        int present = mst_read(d, 2, 0x0b) & 0x08;
        uint8_t db1 = (uint8_t)mst_read(d, 2, 0x48), db2 = (uint8_t)mst_read(d, 2, 0x49);
        uint8_t db3 = (uint8_t)mst_read(d, 2, 0x4a);
        s->src = hd60s_parse_avi(present, db1, db2, db3);
        p->wide = present && (db2 >> 4 & 3) == 2;
        if (source_is_dvi(d) != d->audio_routed_dvi)
            apply_audio(d);
    }
    struct hd60s_colour_cfg want = d->colour;
    int sd = parser_is_sd(p->last_w, p->last_h);
    if (sd >= 0)
        want.sd = sd;
    want.input = s->src.format;
    want.range = d->settings.range;
    want.quant = s->src.quant;
    want.picture = d->settings.picture;
    if (memcmp(&want, &d->colour, sizeof want)) {
        colour_convert(d, &want);
        d->colour = want;
    }
    p->input_format = d->colour.input;
    p->colour = hd60s_frame_colour(&d->colour, s->src.colorimetry);
    if (now_ms - s->win_start >= MODE_WINDOW_MS) {
        double fps = s->frames * (s->interlaced ? 2000.0 : 1000.0) / (double)(now_ms - s->win_start);
        s->mode = s->frames && s->run * 4 >= s->frames * 3 ? tx_mode_find(s->w, s->h, s->interlaced, fps) : NULL;
        s->frames = 0;
        s->win_start = now_ms;
    }
    if (now_ms - s->last_poll >= TX_POLL_MS) {
        struct tx_bus tb = tx_bus(d);
        int hz = hd60s_source_audio_hz(d, s->mode);
        if (hz == s->hz_seen)
            s->hz = hz;
        s->hz_seen = hz;
        tx_poll(&d->tx, &tb, s->mode, s->hz);
        s->last_poll = now_ms;
    }
}

int hd60s_range_from_name(const char *name)
{
    for (int r = RANGE_AUTO; r <= RANGE_EXPANDED; r++)
        if (name && strcmp(name, hd60s_range_names[r]) == 0)
            return r;
    return -1;
}

void hd60s_settings_print(const struct hd60s_settings *s, char *buf, size_t n)
{
    snprintf(buf, n, "{\"range\":\"%s\",\"brightness\":%d,\"contrast\":%d,\"saturation\":%d,\"hue\":%d}",
             hd60s_range_names[s->range], s->picture.brightness, s->picture.contrast, s->picture.saturation, s->picture.hue);
}

/* Value text after "key": in json, or NULL if the key is absent or has no colon. */
static const char *json_value(const char *json, const char *key)
{
    char pat[24];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p)
        return NULL;
    p += strlen(pat);
    p += strspn(p, " \t\r\n");
    if (*p++ != ':')
        return NULL;
    return p + strspn(p, " \t\r\n");
}

/* An absent key leaves *out alone; a present one must be an integer in -128..127. */
static int json_picture_byte(const char *json, const char *key, int8_t *out)
{
    const char *p = json_value(json, key);
    if (!p)
        return strstr(json, key) ? -1 : 0;
    char *end;
    long v = strtol(p, &end, 10);
    if (end == p || v < -128 || v > 127)
        return -1;
    *out = (int8_t)v;
    return 0;
}

int hd60s_settings_parse(const char *json, struct hd60s_settings *out)
{
    const char *p = json ? json_value(json, "range") : NULL;
    if (!p || *p++ != '"')
        return -1;
    const char *end = strchr(p, '"');
    char name[16];
    if (!end || (size_t)(end - p) >= sizeof name)
        return -1;
    memcpy(name, p, (size_t)(end - p));
    name[end - p] = 0;
    int r = hd60s_range_from_name(name);
    struct hd60s_picture pic = {0};
    if (r < 0 || json_picture_byte(json, "brightness", &pic.brightness) || json_picture_byte(json, "contrast", &pic.contrast) ||
        json_picture_byte(json, "saturation", &pic.saturation) || json_picture_byte(json, "hue", &pic.hue))
        return -1;
    out->range = (enum hd60s_range)r;
    out->picture = pic;
    return 0;
}

int hd60s_transfer_parse(const char *s, enum hd60s_transfer *out)
{
    if (strcmp(s, "bulk") == 0)
        *out = HD60S_BULK;
    else if (strcmp(s, "iso") == 0 || strcmp(s, "isochronous") == 0)
        *out = HD60S_ISO;
    else
        return 0;
    return 1;
}

int hd60s_xfer_plan(enum hd60s_transfer mode, const int alt_bytes[HD60S_ISO_ALTS], struct hd60s_xfer_plan *out)
{
    if (mode == HD60S_BULK) {
        *out = (struct hd60s_xfer_plan){.alt = 4, .buffers = 16};
        return 0;
    }
    int best = -1;
    for (int i = 0; i < HD60S_ISO_ALTS; i++)
        if (alt_bytes[i] >= HD60S_ISO_MIN_BYTES && (best < 0 || alt_bytes[i] < alt_bytes[best]))
            best = i;
    if (best < 0)
        return -1;
    *out = (struct hd60s_xfer_plan){.alt = best + 1, .buffers = 4, .packets = 160, .packet_bytes = alt_bytes[best]};
    return 0;
}
