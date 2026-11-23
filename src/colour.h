#ifndef HD60S_COLOUR_H
#define HD60S_COLOUR_H

#include <stdint.h>

#include "parser.h"

/* Picture controls as offsets from Elgato's defaults (device bytes 0x81, 0x80, 0x80, 0x80), so zero is neutral. */
struct hd60s_picture {
    int8_t brightness, contrast, saturation, hue;
};

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

#endif
