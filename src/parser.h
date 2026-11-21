#ifndef HD60S_PARSER_H
#define HD60S_PARSER_H

#include <stddef.h>
#include <stdint.h>

/* Width in pixels, height in lines; frame is YUY2 (Y Cb Y Cr), stride = width * 2. */
typedef void (*frame_cb)(void *ctx, const uint8_t *frame, int width, int height, int interlaced);
typedef void (*audio_cb)(void *ctx, const uint8_t *pcm, size_t len);

struct hd60s_parser {
    uint8_t *buf;
    size_t cap;
    frame_cb on_frame;
    audio_cb on_audio;
    void *ctx;

    uint8_t carry[4];
    int carry_len;
    size_t audio_left;

    int prev_v;     /* -1 until the first timing code */
    uint8_t fields; /* field-bit history, newest in bit 0 */
    int capturing, interlaced;
    int in_line, line, step, lines, long_lines;
    size_t line_off, stride, line_max;
    int frame_bad;
    unsigned long frames, discarded, long_total;
};

void parser_init(struct hd60s_parser *p, uint8_t *buf, size_t cap, frame_cb on_frame,
                 audio_cb on_audio, void *ctx);
void parser_feed(struct hd60s_parser *p, const uint8_t *data, size_t len);

#endif
