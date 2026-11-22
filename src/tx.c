#include "tx.h"

#include <math.h>
#include <string.h>

enum { INIT_MARK = 0xb2, EDID_RETRY_POLLS = 8 };

static const struct {
    int w, h, interlaced, fps;
    struct tx_mode m;
} modes[] = {
    {720, 480, 1, 60, {6, 1, 0}},    {720, 576, 1, 50, {21, 1, 0}},   {1280, 720, 0, 60, {4, 0, 1}},
    {1280, 720, 0, 50, {19, 0, 1}},  {1920, 1080, 1, 60, {5, 0, 1}},  {1920, 1080, 1, 50, {20, 0, 1}},
    {1920, 1080, 0, 30, {34, 0, 1}}, {1920, 1080, 0, 25, {33, 0, 1}}, {1920, 1080, 0, 24, {32, 0, 1}},
    {1920, 1080, 0, 60, {16, 0, 1}}, {1920, 1080, 0, 50, {31, 0, 1}}, {720, 480, 0, 60, {2, 0, 0}},
    {720, 576, 0, 50, {17, 0, 0}},   {640, 480, 0, 60, {1, 0, 0}},
};

const struct tx_mode *tx_mode_find(int width, int height, int interlaced, double fps)
{
    const struct tx_mode *best = NULL;
    double bd = 3.0;
    for (size_t i = 0; i < sizeof modes / sizeof *modes; i++) {
        double d = fabs(fps - modes[i].fps);
        if (modes[i].w == width && modes[i].h == height && modes[i].interlaced == !!interlaced && d <= bd) {
            best = &modes[i].m;
            bd = d;
        }
    }
    return best;
}

static int block_ok(const uint8_t *b)
{
    unsigned s = 0;
    for (int i = 0; i < 128; i++)
        s += b[i];
    return (s & 0xff) == 0;
}

int tx_edid_parse(const uint8_t *e, size_t len, struct tx_sink *out)
{
    static const uint8_t hdr[8] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    memset(out, 0, sizeof *out);
    if (len < 128 || memcmp(e, hdr, 8) || !block_ok(e))
        return 0;
    out->valid = 1;
    for (size_t n = 1; n <= e[0x7e] && n <= 4 && (n + 1) * 128 <= len; n++) {
        const uint8_t *x = e + n * 128;
        if (x[0] != 2 || !block_ok(x))
            continue;
        out->ycc444 |= !!(x[3] & 0x20);
        out->ycc422 |= !!(x[3] & 0x10);
        for (int i = 4; i < x[2] && i < 127; i += (x[i] & 31) + 1)
            if ((x[i] >> 5) == 3 && (x[i] & 31) >= 5 && x[i + 1] == 0x03 && x[i + 2] == 0x0c && x[i + 3] == 0)
                out->hdmi = 1;
    }
    return 1;
}

void tx_avi(uint8_t out[6], enum tx_colour c, const struct tx_mode *m)
{
    /* Always BT.709: the capture matrix is never switched to BT.601 for SD. */
    out[0] = (uint8_t)(0x10 | c << 5);
    out[1] = (uint8_t)(0x88 | (m->wide ? 0x20 : 0x10));
    out[2] = 0;
    out[3] = m->vic;
    out[4] = m->rep;
    out[5] = (uint8_t)(0x6f - (out[0] + out[1] + out[3] + out[4]));
}

enum tx_state tx_next(enum tx_state s, struct tx_in in)
{
    if (!in.hpd)
        return TX_IDLE;
    switch (s) {
    case TX_IDLE: return TX_EDID;
    case TX_EDID: return in.edid_ok ? TX_PICK : TX_IDLE;
    case TX_PICK: return in.blank ? TX_PICK : TX_VIDEO;
    case TX_UNSTABLE:
        if (in.blank || !in.stable)
            return TX_UNSTABLE;
        return in.mode_changed ? TX_VIDEO : TX_ENABLE;
    case TX_VIDEO: return in.stable ? TX_ENABLE : TX_UNSTABLE;
    case TX_ENABLE: return TX_RUNNING;
    case TX_RUNNING:
        if (in.blank)
            return TX_RUNNING;
        if (!in.stable)
            return TX_UNSTABLE;
        return in.mode_changed ? TX_VIDEO : TX_RUNNING;
    }
    return TX_IDLE;
}

/* Write allow-list: transmitter video, DDC read and mute registers only. No HDCP (0x1F-0x46), audio (0xE0+) or burst DDC. */
int tx_writable(uint8_t r, uint8_t v)
{
    switch (r) {
    case 0x10: return v == 1;
    case 0x11: return v == 0xa0;
    case 0x15: return v == 0 || v == 3 || v == 9 || v == 0x0f;
    case 0x65: return v == 0 || v == 2;
    case 0x0f: case 0xc0: case 0xc1: case 0xc6: return 1;
    }
    return (r >= 0x04 && r <= 0x05) || (r >= 0x09 && r <= 0x0b) || (r >= 0x12 && r <= 0x14) ||
           (r >= 0x58 && r <= 0x68) || (r >= 0x70 && r <= 0x87) || (r >= 0x90 && r <= 0xa3) ||
           (r >= 0xc9 && r <= 0xd0);
}

int tx_write(const struct tx_bus *b, uint8_t r, uint8_t v)
{
    if (!tx_writable(r, v))
        return -1;
    b->write(b->ctx, r, v);
    return 0;
}

static void w(const struct tx_bus *b, uint8_t r, uint8_t v) { tx_write(b, r, v); }
static int rd(const struct tx_bus *b, uint8_t r) { return b->read(b->ctx, r); }

static void rmw(const struct tx_bus *b, uint8_t r, uint8_t mask, uint8_t v)
{
    w(b, r, (uint8_t)((rd(b, r) & ~mask) | v));
}

static void mute(const struct tx_bus *b)
{
    w(b, 0x0f, 0);
    w(b, 0xc1, 1);
    w(b, 0xc6, 3);
}

static void unmute(const struct tx_bus *b)
{
    w(b, 0x0f, 0);
    w(b, 0xc1, 0);
    w(b, 0xc6, 3);
}

static void afe_off(struct tx *t, const struct tx_bus *b)
{
    w(b, 0x04, 0x1d);
    w(b, 0x61, 0x30);
    t->sent_vic = 0;
    memset(&t->sink, 0, sizeof t->sink);
}

static void chip_init(const struct tx_bus *b)
{
    w(b, 0x05, 0);
    w(b, 0x04, 0x3d);
    b->sleep_ms(b->ctx, 5);
    w(b, 0x04, 0x1d);
    w(b, 0x61, 0x30);
    w(b, 0x09, INIT_MARK);
    w(b, 0x0a, 0xf8);
    w(b, 0x0b, 0x37);
    for (uint8_t r = 0xc9; r <= 0xd0; r++)
        w(b, r, 0);
}

static int ddc_chunk(const struct tx_bus *b, int seg, int off, uint8_t *out)
{
    int ok = 0;
    w(b, 0x10, 1);
    w(b, 0x15, 0x09);
    w(b, 0x65, 2);
    w(b, 0x11, 0xa0);
    w(b, 0x12, (uint8_t)off);
    w(b, 0x13, 32);
    w(b, 0x14, (uint8_t)seg);
    w(b, 0x15, 0x03);
    for (int i = 0; i < 50; i++) {
        int s = rd(b, 0x16);
        if (s & 0x28)
            break;
        if (s & 0x80) {
            ok = b->read_n(b->ctx, 0x17, out, 32) >= 0;
            break;
        }
        b->sleep_ms(b->ctx, 1);
    }
    if (!ok)
        w(b, 0x15, 0x0f);
    w(b, 0x65, 0);
    return ok;
}

static int read_edid(const struct tx_bus *b, struct tx_sink *sink)
{
    uint8_t e[640];
    size_t n = 1;
    for (size_t blk = 0; blk < n; blk++)
        for (int c = 0; c < 4; c++)
            if (!ddc_chunk(b, (int)(blk >> 1), (int)(blk & 1) * 128 + c * 32, e + blk * 128 + (size_t)c * 32))
                return 0;
            else if (blk == 0 && c == 3)
                n = 1 + (e[0x7e] > 4 ? 4 : e[0x7e]);
    return tx_edid_parse(e, n * 128, sink);
}

static const uint8_t csc_bt709[18] = {0x4f, 0x09, 0xba, 0x3b, 0x4b, 0x3e, 0x4f, 0x09, 0x56,
                                      0x0e, 0x00, 0x00, 0x4f, 0x09, 0x00, 0x00, 0xe7, 0x10};

static void program_video(struct tx *t, const struct tx_bus *b)
{
    const struct tx_mode *m = t->want;
    int hdmi = t->sink.hdmi, hs = m->vic == 16 || m->vic == 31;
    w(b, 0x04, 0x1d);
    if (hdmi)
        mute(b);
    w(b, 0x70, (uint8_t)((rd(b, 0x70) & 0x23) | 0x40));
    w(b, 0x90, 0);
    for (uint8_t r = 0x91; r <= 0xa3; r++)
        w(b, r, 0xff);
    if (t->colour == TX_RGB) {
        w(b, 0x73, 0x10);
        w(b, 0x74, 0x80);
        w(b, 0x75, 0x10);
        for (uint8_t i = 0; i < 18; i++)
            w(b, (uint8_t)(0x76 + i), csc_bt709[i]);
        w(b, 0x72, (uint8_t)(3 | (rd(b, 0x72) & 0x1c)));
        rmw(b, 0x0f, 0x10, 0);
    } else {
        w(b, 0x72, (uint8_t)(rd(b, 0x72) & 0x1c));
        rmw(b, 0x0f, 0x10, 0x10);
    }
    w(b, 0xc0, hdmi);
    w(b, 0x0f, 1);
    w(b, 0x58, hdmi ? 0x10 : 0);
    w(b, 0x0f, 0);
    rmw(b, 0xc1, 3, hdmi ? 3 : 2);
    rmw(b, 0xc6, 3, hdmi ? 3 : 0);
    rmw(b, 0x71, 0xfc, 0x1c);
    w(b, 0x61, 0x10);
    rmw(b, 0x62, 0x90, hs ? 0x80 : 0x10);
    rmw(b, 0x64, 0x89, hs ? 0x80 : 0x09);
    rmw(b, 0x68, 0x10, hs ? 0x80 : 0x10);
    w(b, 0x04, (uint8_t)(rd(b, 0x04) & 0xc5));
    w(b, 0x59, (uint8_t)(rd(b, 0x59) | 0x08));
    if (hdmi) {
        uint8_t a[6];
        tx_avi(a, t->colour, m);
        w(b, 0x0f, 1);
        for (uint8_t i = 0; i < 5; i++)
            w(b, (uint8_t)(0x58 + i), a[i]);
        w(b, 0x5d, a[5]);
        for (uint8_t r = 0x5e; r <= 0x65; r++)
            w(b, r, 0);
        w(b, 0x0f, 0);
        w(b, 0xcd, 3);
    }
    w(b, 0x04, (uint8_t)(rd(b, 0x04) & ~0x08));
    t->sent_vic = m->vic;
}

static void enable_output(const struct tx_bus *b)
{
    mute(b);
    w(b, 0x0f, 0);
    w(b, 0x61, 0x00);
    unmute(b);
}

static struct tx_in act(struct tx *t, const struct tx_bus *b, struct tx_in in)
{
    switch (t->state) {
    case TX_IDLE:
        if (!in.hpd)
            t->edid_wait = 0;
        else if (t->edid_wait) {
            t->edid_wait--;
            in.hpd = 0;
        }
        break;
    case TX_EDID:
        in.edid_ok = read_edid(b, &t->sink);
        if (!in.edid_ok)
            t->edid_wait = EDID_RETRY_POLLS;
        break;
    case TX_PICK:
        t->colour = !t->sink.hdmi ? TX_RGB : t->sink.ycc444 ? TX_YCC444 : t->sink.ycc422 ? TX_YCC422 : TX_RGB;
        break;
    case TX_VIDEO:
        program_video(t, b);
        for (int i = 0; i < 50 && !(rd(b, 0x0e) & 0x10); i++)
            b->sleep_ms(b->ctx, 10);
        in.stable = !!(rd(b, 0x0e) & 0x10);
        break;
    case TX_ENABLE: enable_output(b); break;
    case TX_RUNNING:
        if (!in.stable && !in.blank)
            mute(b);
        break;
    case TX_UNSTABLE: break;
    }
    return in;
}

void tx_start(struct tx *t, const struct tx_bus *b)
{
    memset(t, 0, sizeof *t);
    t->present = rd(b, 1) == 0x49 && rd(b, 2) == 0x12 && rd(b, 3) == 0x16;
    if (t->present && rd(b, 0x09) != INIT_MARK)
        chip_init(b);
}

void tx_poll(struct tx *t, const struct tx_bus *b, const struct tx_mode *mode)
{
    if (!t->present)
        return;
    if (rd(b, 0x09) != INIT_MARK) {
        chip_init(b);
        mute(b);
        t->state = TX_IDLE;
        t->sent_vic = 0;
    }
    t->want = mode;
    if (!mode && t->sent_vic) {
        mute(b);
        t->sent_vic = 0;
    }
    for (int i = 0; i < 8; i++) {
        int v = rd(b, 0x0e);
        struct tx_in in = {.hpd = !!(v & 0x40),
                           .stable = !!(v & 0x10),
                           .blank = !mode,
                           .mode_changed = mode && mode->vic != t->sent_vic};
        enum tx_state s = t->state, next;
        in = act(t, b, in);
        next = tx_next(s, in);
        if (next == TX_IDLE && s != TX_IDLE && !in.hpd)
            afe_off(t, b);
        t->state = next;
        if (next == s)
            break;
    }
}

void tx_blank(struct tx *t, const struct tx_bus *b)
{
    if (t->present)
        mute(b);
    t->sent_vic = 0;
}
