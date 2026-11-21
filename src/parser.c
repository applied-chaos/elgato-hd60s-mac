#include "parser.h"

#include <string.h>

void parser_init(struct hd60s_parser *p, uint8_t *buf, size_t cap, frame_cb on_frame,
                 audio_cb on_audio, void *ctx)
{
    memset(p, 0, sizeof *p);
    p->buf = buf;
    p->cap = cap;
    p->on_frame = on_frame;
    p->on_audio = on_audio;
    p->ctx = ctx;
    p->prev_v = -1;
}

static int leaked_blanking(const struct hd60s_parser *p)
{
    return p->stride && p->line_off > p->stride && p->line_off < 2 * p->stride;
}

static void end_line(struct hd60s_parser *p)
{
    if (!p->in_line)
        return;
    p->in_line = 0;
    /* A corrupted EAV lets horizontal blanking through after intact video; video() already capped it at stride. */
    if (leaked_blanking(p)) {
        p->long_lines++;
        p->long_total++;
    } else if (p->line_off != p->stride)
        p->frame_bad = 1;
    p->line_max = p->line_off;
}

static void begin_frame(struct hd60s_parser *p, int interlaced)
{
    p->interlaced = interlaced;
    p->stride = p->line_max;
    p->frame_bad = p->stride == 0;
    p->lines = 0;
    p->long_lines = 0;
}

/* Field history decides layout, as Elgato does: 0x00/0xFF progressive, 0xAA even field, 0x55 odd field. */
static void begin_active(struct hd60s_parser *p)
{
    p->capturing = 1;
    switch (p->fields) {
    case 0x00:
    case 0xff:
        begin_frame(p, 0);
        p->line = 0;
        p->step = 1;
        break;
    case 0xaa:
        begin_frame(p, 1);
        p->line = 0;
        p->step = 2;
        break;
    case 0x55:
        if (!p->interlaced)
            begin_frame(p, 1);
        p->line = 1;
        p->step = 2;
        break;
    default:
        p->capturing = 0;
    }
}

static int mode_changed(const struct hd60s_parser *p) { return p->long_lines * 2 > p->lines; }

static void end_active(struct hd60s_parser *p)
{
    int complete = !p->interlaced || p->fields == 0x55;
    if (mode_changed(p))
        p->frame_bad = 1;
    else if (!p->frame_bad)
        p->line_max = p->stride;
    if (p->capturing && complete) {
        if (!p->frame_bad && p->lines > 0) {
            p->frames++;
            p->on_frame(p->ctx, p->buf, (int)(p->stride / 2), p->lines, p->interlaced);
        } else if (p->stride) {
            p->discarded++;
        }
    }
    p->capturing = 0;
}

static void timing_code(struct hd60s_parser *p, uint8_t xy)
{
    int v = (xy & 0x20) != 0, f = (xy & 0x40) != 0;
    end_line(p);
    if (p->prev_v == 1 && !v) {
        p->fields = (uint8_t)(p->fields << 1 | f);
        begin_active(p);
    } else if (p->prev_v == 0 && v) {
        end_active(p);
    }
    p->prev_v = v;
    if (!v && p->capturing) {
        p->in_line = 1;
        p->line_off = 0;
        if ((size_t)(p->line + 1) * p->stride > p->cap)
            p->frame_bad = 1;
        else if (p->line + 1 > p->lines)
            p->lines = p->line + 1;
        p->line += p->step;
    }
}

static void video(struct hd60s_parser *p, const uint8_t *d, size_t n)
{
    if (!p->in_line)
        return;
    if (!p->frame_bad && p->line_off < p->stride) {
        size_t room = p->stride - p->line_off;
        size_t row = (size_t)(p->line - p->step);
        memcpy(p->buf + row * p->stride + p->line_off, d, n < room ? n : room);
    }
    p->line_off += n;
}

static int is_control(const uint8_t *w) { return w[0] == 0xff && w[1] == 0x00 && (w[2] == 0x00 || w[2] == 0xff); }

static void word(struct hd60s_parser *p, const uint8_t *w)
{
    if (!is_control(w)) {
        video(p, w, 4);
    } else if (w[2] == 0x00) {
        timing_code(p, w[3]);
    } else {
        end_line(p);
        p->audio_left = (size_t)w[3] * 4;
    }
}

void parser_feed(struct hd60s_parser *p, const uint8_t *d, size_t len)
{
    while (len) {
        if (p->audio_left) {
            size_t n = len < p->audio_left ? len : p->audio_left;
            p->on_audio(p->ctx, d, n);
            p->audio_left -= n;
            d += n;
            len -= n;
        } else if (p->carry_len || len < 4) {
            size_t n = (size_t)(4 - p->carry_len);
            if (n > len)
                n = len;
            memcpy(p->carry + p->carry_len, d, n);
            p->carry_len += (int)n;
            d += n;
            len -= n;
            if (p->carry_len == 4) {
                p->carry_len = 0;
                word(p, p->carry);
            }
        } else if (is_control(d)) {
            word(p, d);
            d += 4;
            len -= 4;
        } else {
            size_t n = 4;
            while (n + 4 <= len && !is_control(d + n))
                n += 4;
            video(p, d, n);
            d += n;
            len -= n;
        }
    }
}
