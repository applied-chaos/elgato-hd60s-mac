#include "colour.h"

#include <string.h>

#include "ub530_tables.h"

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
