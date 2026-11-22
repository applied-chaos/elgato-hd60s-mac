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
    void *ctx;       /* frame callback context */
    void *audio_ctx; /* audio callback context; stays put when ctx is swapped */

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
    int last_w, last_h; /* size of the last frame emitted */
    int wide;           /* source AVI InfoFrame flags 16:9; set by hd60s_stream, about once a second */
};

/* 1 for an SD frame size (640x480, 720x480/576, 1440x480), 0 for HD and PC/DMT sizes, -1 otherwise (runts
 * while the source switches mode, or a timing with no known mode). */
int parser_is_sd(int width, int height);

void parser_init(struct hd60s_parser *p, uint8_t *buf, size_t cap, frame_cb on_frame,
                 audio_cb on_audio, void *ctx);
void parser_feed(struct hd60s_parser *p, const uint8_t *data, size_t len);

#endif
