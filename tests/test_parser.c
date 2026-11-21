#include "../src/parser.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 8
#define H 6

struct sink {
    int frames, last_w, last_h, last_interlaced;
    uint8_t last[W * 2 * H];
    uint8_t audio[256];
    size_t audio_len;
};

static void on_frame(void *ctx, const uint8_t *f, int w, int h, int interlaced)
{
    struct sink *s = ctx;
    s->frames++;
    s->last_w = w;
    s->last_h = h;
    s->last_interlaced = interlaced;
    if (w == W && h == H)
        memcpy(s->last, f, sizeof s->last);
}

static void on_audio(void *ctx, const uint8_t *pcm, size_t len)
{
    struct sink *s = ctx;
    assert(s->audio_len + len <= sizeof s->audio);
    memcpy(s->audio + s->audio_len, pcm, len);
    s->audio_len += len;
}

struct stream {
    uint8_t *b;
    size_t n, cap;
};

static void put(struct stream *s, const void *p, size_t n)
{
    if (s->n + n > s->cap) {
        s->cap = (s->n + n) * 2;
        s->b = realloc(s->b, s->cap);
    }
    memcpy(s->b + s->n, p, n);
    s->n += n;
}

static void code(struct stream *s, uint8_t xy)
{
    uint8_t c[4] = {0xff, 0x00, 0x00, xy};
    put(s, c, 4);
}

static uint8_t pix(int line, int x) { return (uint8_t)(0x10 + line * 16 + x); }

#define NO_BLANKING (-1)

/* One field or progressive frame: blanking, active lines, then blanking again. */
static void field_ex(struct stream *s, int f, int first_line, int step, int lines, int w, int blanking_after_line)
{
    uint8_t fb = f ? 0x40 : 0;
    code(s, 0x20 | fb);
    put(s, (uint8_t[8]){0x80, 0x10, 0x80, 0x10, 0x80, 0x10, 0x80, 0x10}, 8);
    code(s, 0x20 | fb);
    for (int i = 0; i < lines; i++) {
        uint8_t line[64];
        for (int x = 0; x < w * 2; x++)
            line[x] = pix(first_line + i * step, x);
        code(s, fb);
        put(s, line, (size_t)w * 2);
        if (i == blanking_after_line)
            put(s, (uint8_t[8]){0x10, 0x80, 0x10, 0x80, 0x10, 0x80, 0x10, 0x80}, 8);
    }
    code(s, 0x20 | fb);
}

static void field(struct stream *s, int f, int first_line, int step, int lines)
{
    field_ex(s, f, first_line, step, lines, W, NO_BLANKING);
}

static void feed_chunked(struct hd60s_parser *p, struct stream *s, size_t chunk)
{
    for (size_t o = 0; o < s->n; o += chunk)
        parser_feed(p, s->b + o, s->n - o < chunk ? s->n - o : chunk);
}

static void check_progressive_frame(uint8_t *last)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W * 2; x++)
            assert(last[y * W * 2 + x] == pix(y, x));
}

static void test_progressive(size_t chunk)
{
    static uint8_t buf[1 << 16];
    struct sink s = {0};
    struct hd60s_parser p;
    parser_init(&p, buf, sizeof buf, on_frame, on_audio, &s);
    struct stream st = {0};
    for (int i = 0; i < 3; i++)
        field(&st, 0, 0, 1, H);
    feed_chunked(&p, &st, chunk);
    assert(s.frames >= 2);
    assert(s.last_w == W && s.last_h == H && !s.last_interlaced);
    check_progressive_frame(s.last);
    free(st.b);
}

static void test_interlaced(void)
{
    static uint8_t buf[1 << 16];
    struct sink s = {0};
    struct hd60s_parser p;
    parser_init(&p, buf, sizeof buf, on_frame, on_audio, &s);
    struct stream st = {0};
    for (int i = 0; i < 12; i++) {
        int f = i & 1;
        field(&st, f, f, 2, H / 2);
    }
    feed_chunked(&p, &st, 7);
    assert(s.frames >= 1);
    assert(s.last_w == W && s.last_h == H && s.last_interlaced);
    check_progressive_frame(s.last);
    free(st.b);
}

static void test_audio(void)
{
    static uint8_t buf[1 << 16];
    struct sink s = {0};
    struct hd60s_parser p;
    parser_init(&p, buf, sizeof buf, on_frame, on_audio, &s);
    struct stream st = {0};
    /* Payload deliberately contains a timing-code pattern that must not be treated as one. */
    uint8_t pcm[8] = {0xff, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04};
    for (int i = 0; i < 3; i++) {
        field(&st, 0, 0, 1, H);
        put(&st, (uint8_t[4]){0xff, 0x00, 0xff, 0x02}, 4);
        put(&st, pcm, sizeof pcm);
    }
    feed_chunked(&p, &st, 3);
    assert(s.audio_len == 3 * sizeof pcm);
    for (int i = 0; i < 3; i++)
        assert(memcmp(s.audio + i * sizeof pcm, pcm, sizeof pcm) == 0);
    assert(s.frames >= 2);
    check_progressive_frame(s.last);
    free(st.b);
}

static void test_corrupt_eav_keeps_frame_and_stride(int blanking_after_line)
{
    static uint8_t buf[1 << 16];
    struct sink s = {0};
    struct hd60s_parser p;
    parser_init(&p, buf, sizeof buf, on_frame, on_audio, &s);
    struct stream st = {0};
    field(&st, 0, 0, 1, H);
    field_ex(&st, 0, 0, 1, H, W, blanking_after_line);
    feed_chunked(&p, &st, 1 << 20);
    assert(s.frames == 1);
    check_progressive_frame(s.last);
    size_t fed = st.n;
    field(&st, 0, 0, 1, H);
    parser_feed(&p, st.b + fed, st.n - fed);
    assert(s.frames == 2);
    assert(p.long_total == 1 && p.discarded == 0);
    check_progressive_frame(s.last);
    free(st.b);
}

static void test_lost_sav_merges_lines_drops_frame(void)
{
    static uint8_t buf[1 << 16];
    struct sink s = {0};
    struct hd60s_parser p;
    parser_init(&p, buf, sizeof buf, on_frame, on_audio, &s);
    struct stream st = {0};
    field(&st, 0, 0, 1, H);
    field(&st, 0, 0, 1, H);
    code(&st, 0x20);
    code(&st, 0x20);
    for (int i = 0; i < H; i++) {
        uint8_t line[W * 2];
        for (int x = 0; x < W * 2; x++)
            line[x] = pix(i, x);
        if (i != 3)
            code(&st, 0);
        put(&st, line, sizeof line);
    }
    code(&st, 0x20);
    field(&st, 0, 0, 1, H);
    feed_chunked(&p, &st, 1 << 20);
    assert(s.frames == 2);
    assert(s.last_h == H);
    assert(p.discarded == 1);
    check_progressive_frame(s.last);
    free(st.b);
}

static void test_mode_change(void)
{
    static uint8_t buf[1 << 16];
    struct sink s = {0};
    struct hd60s_parser p;
    parser_init(&p, buf, sizeof buf, on_frame, on_audio, &s);
    struct stream st = {0};
    for (int i = 0; i < 2; i++)
        field(&st, 0, 0, 1, H);
    for (int i = 0; i < 3; i++)
        field_ex(&st, 0, 0, 1, H, W + 4, NO_BLANKING);
    feed_chunked(&p, &st, 1 << 20);
    assert(s.last_w == W + 4 && s.last_h == H);
    free(st.b);
}

int main(void)
{
    test_progressive(1 << 20);
    test_progressive(1);
    test_progressive(5);
    test_interlaced();
    test_audio();
    test_corrupt_eav_keeps_frame_and_stride(2);
    test_corrupt_eav_keeps_frame_and_stride(H - 1);
    test_lost_sav_merges_lines_drops_frame();
    test_mode_change();
    puts("parser tests passed");
    return 0;
}
