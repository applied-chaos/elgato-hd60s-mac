#include "tx.h"

#include <string.h>

enum { INIT_MARK = 0xb2, EDID_RETRY_POLLS = 8, HDCP_BACKOFF_POLLS = 8, HDCP_KSV_ONES = 20, DDC_POLLS = 200, DDC_DONE = 0x80, DDC_ERR_MASK = 0x38, HD_MIN_PCLK_KHZ = 74250, AVI_C1C0_BT601 = 0x48, AVI_C1C0_BT709 = 0x88 };

static const struct {
    int w, h, interlaced, fps;
    struct tx_mode m;
} modes[] = {
    {720, 480, 1, 60, {6, 1, 0, 27000}},    {720, 576, 1, 50, {21, 1, 0, 27000}},   {1280, 720, 0, 60, {4, 0, 1, 74250}},
    {1280, 720, 0, 50, {19, 0, 1, 74250}},  {1920, 1080, 1, 60, {5, 0, 1, 74250}},  {1920, 1080, 1, 50, {20, 0, 1, 74250}},
    {1920, 1080, 0, 30, {34, 0, 1, 74250}}, {1920, 1080, 0, 25, {33, 0, 1, 74250}}, {1920, 1080, 0, 24, {32, 0, 1, 74250}},
    {1920, 1080, 0, 60, {16, 0, 1, 148500}}, {1920, 1080, 0, 50, {31, 0, 1, 148500}}, {720, 480, 0, 60, {2, 0, 0, 27000}},
    {720, 576, 0, 50, {17, 0, 0, 27000}},   {640, 480, 0, 60, {1, 0, 0, 25175}},
};

const struct tx_mode *tx_mode_find(int width, int height, int interlaced, double fps)
{
    const struct tx_mode *best = NULL;
    double bd = 3.0;
    for (size_t i = 0; i < sizeof modes / sizeof *modes; i++) {
        double d = fps > modes[i].fps ? fps - modes[i].fps : modes[i].fps - fps;
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
        out->basic_audio |= !!(x[3] & 0x40);
        for (int i = 4; i < x[2] && i < 127; i += (x[i] & 31) + 1)
            if ((x[i] >> 5) == 3 && (x[i] & 31) >= 5 && x[i + 1] == 0x03 && x[i + 2] == 0x0c && x[i + 3] == 0)
                out->hdmi = 1;
    }
    return 1;
}

unsigned tx_audio_n(uint32_t pclk_khz, int hz)
{
    /* HDMI 1.4 table 7-1..7-3; every 27, 74.25 and 148.5 MHz row shares the same N. */
    static const unsigned vga[3] = {4576, 7007, 6864}, std[3] = {4096, 6272, 6144};
    int i = hz == 32000 ? 0 : hz == 44100 ? 1 : hz == 48000 ? 2 : -1;
    if (i < 0)
        return 0;
    return pclk_khz < 26000 ? vga[i] : std[i];
}

int tx_audio_fs_code(int hz)
{
    return hz == 44100 ? 0 : hz == 48000 ? 2 : hz == 32000 ? 3 : -1;
}

int tx_audio_rate(uint32_t pclk_khz, uint32_t n, uint32_t cts)
{
    static const int std[] = {32000, 44100, 48000, 88200, 96000, 176400, 192000};
    if (!n || !cts)
        return 0;
    double fs = (double)pclk_khz * 1000.0 * n / (128.0 * cts);
    for (size_t i = 0; i < sizeof std / sizeof *std; i++)
        if ((fs > std[i] ? fs - std[i] : std[i] - fs) < std[i] * 0.02)
            return std[i];
    return 0;
}

void tx_avi(uint8_t out[6], enum tx_colour c, const struct tx_mode *m)
{
    int bt601 = m->pclk_khz < HD_MIN_PCLK_KHZ;
    out[0] = (uint8_t)(0x10 | c << 5);
    out[1] = (uint8_t)((bt601 ? AVI_C1C0_BT601 : AVI_C1C0_BT709) | (m->wide ? 0x20 : 0x10));
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

enum tx_hdcp_op tx_hdcp_next(enum tx_hdcp s, int want, int retry_polls_left)
{
    static const enum tx_hdcp_op ops[3][2] = {
        [HDCP_OFF] = {HDCP_NOP, HDCP_AUTH},
        [HDCP_ON] = {HDCP_STOP, HDCP_NOP},
        [HDCP_BACKOFF] = {HDCP_STOP, HDCP_AUTH},
    };
    if (s == HDCP_BACKOFF && want && retry_polls_left)
        return HDCP_NOP;
    return ops[s][!!want];
}

/* Write allow-list: transmitter video, audio, DDC read, mute and HDCP registers only. DDC is read-only: slaves 0xA0 and 0x74. */
int tx_writable(uint8_t r, uint8_t v)
{
    switch (r) {
    case 0x10: return v <= 1;
    case 0x11: return v == 0xa0 || v == 0x74;
    case 0x0c: return v == 0x1c || v == 4;
    case 0x0d: case 0x22: return v == 0;
    case 0x0e: case 0x1f: case 0x20: return v <= 1;
    case 0x21: return v == 1;
    case 0x15: return v == 0 || v == 3 || v == 9 || v == 0x0f;
    case 0x65: return v == 0 || v == 2;
    case 0x0f: case 0xc0: case 0xc1: case 0xc6: return 1;
    case 0xc5: return v == 0;
    case 0xce: return v == 0 || v == 3;
    case 0xe0: return v == 0xc1 || v == 0;
    case 0xe1: return v == 0x01;
    case 0xe2: return v == 0xe4;
    case 0xe3: return v == 0;
    case 0xe4: return v == 0x08;
    }
    return (r >= 0x04 && r <= 0x05) || (r >= 0x28 && r <= 0x2f) || (r >= 0x09 && r <= 0x0b) || (r >= 0x12 && r <= 0x14) ||
           (r >= 0x33 && r <= 0x35) || (r >= 0x58 && r <= 0x6d) || (r >= 0x70 && r <= 0x87) || (r >= 0x90 && r <= 0xa3) ||
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

static int muted_pending_hdcp(const struct tx *t) { return t->src_hdcp && t->sink.hdmi && t->hdcp != HDCP_ON; }

static void audio_off(struct tx *t, const struct tx_bus *b)
{
    if (!t->audio_hz)
        return;
    w(b, 0xce, 0);
    w(b, 0xe0, 0);
    t->audio_hz = 0;
}

/* Rate to send, 0 = off. Basic audio (2ch PCM at 32/44.1/48 kHz) is mandatory in every sink that advertises it. */
static int audio_wanted(const struct tx *t)
{
    return t->sink.hdmi && t->sink.basic_audio && t->want && tx_audio_n(t->want->pclk_khz, t->want_hz) ? t->want_hz : 0;
}

static void audio_on(struct tx *t, const struct tx_bus *b, int hz)
{
    unsigned n = tx_audio_n(t->want->pclk_khz, hz);
    int fs = tx_audio_fs_code(hz);
    w(b, 0x0f, 1);
    for (uint8_t i = 0; i < 3; i++)
        w(b, (uint8_t)(0x33 + i), (uint8_t)(n >> (8 * i)));
    w(b, 0x0f, 0);
    w(b, 0xc5, 0);
    rmw(b, 0x58, 0x0c, 0x04);
    w(b, 0x04, (uint8_t)(rd(b, 0x04) | 0x04));
    w(b, 0xe2, 0xe4);
    w(b, 0xe3, 0x00);
    w(b, 0xe4, 0x08);
    w(b, 0xe0, 0xc1);
    w(b, 0xe1, 0x01);
    w(b, 0x0f, 1);
    static const uint8_t chst[7] = {0, 0, 1, 0x21, 0x43, 0x65, 0x87};
    for (uint8_t i = 0; i < 7; i++)
        w(b, (uint8_t)(0x91 + i), chst[i]);
    w(b, 0x98, (uint8_t)fs);
    w(b, 0x99, (uint8_t)((~fs & 0xf) << 4 | 0x0b));
    w(b, 0x68, 0x01);
    for (uint8_t r = 0x69; r <= 0x6c; r++)
        w(b, r, 0);
    w(b, 0x6d, 0x70);
    w(b, 0x0f, 0);
    w(b, 0xce, 0x03);
    w(b, 0x04, (uint8_t)(rd(b, 0x04) & ~0x04));
    t->audio_hz = hz;
}

static void audio_sync(struct tx *t, const struct tx_bus *b)
{
    int hz = audio_wanted(t);
    if (hz)
        audio_on(t, b, hz);
    else
        audio_off(t, b);
}

static void afe_off(struct tx *t, const struct tx_bus *b)
{
    audio_off(t, b);
    w(b, 0x04, 0x1d);
    w(b, 0x61, 0x30);
    t->sent_vic = 0;
    t->hdcp = HDCP_OFF;
    t->hdcp_retry_polls_left = 0;
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

static int ddc_wait(const struct tx_bus *b)
{
    for (int i = 0; i < DDC_POLLS; i++) {
        int s = rd(b, 0x16);
        if (s & DDC_ERR_MASK)
            return -1;
        if (s & DDC_DONE)
            return 0;
        b->sleep_ms(b->ctx, 1);
    }
    return -1;
}

static int hdcp_read(const struct tx_bus *b, uint8_t off, uint8_t n)
{
    w(b, 0x10, 1);
    w(b, 0x11, 0x74);
    w(b, 0x12, off);
    w(b, 0x13, n);
    w(b, 0x15, 0);
    return ddc_wait(b);
}

static void ddc_abort(const struct tx_bus *b)
{
    w(b, 0x65, 2);
    w(b, 0x20, 0);
    w(b, 0x04, (uint8_t)(rd(b, 0x04) | 1));
    w(b, 0x10, 1);
    for (int i = 0; i < 2; i++) {
        w(b, 0x15, 0x0f);
        ddc_wait(b);
    }
    w(b, 0x65, 0);
}

static void clear_ints(const struct tx_bus *b)
{
    rmw(b, 0x0a, 7, 0);
    w(b, 0x0c, 0x1c);
    w(b, 0x0d, 0);
    w(b, 0x0e, 1);
}

static void hdcp_stop(const struct tx_bus *b)
{
    w(b, 0x22, 0);
    w(b, 0x20, 0);
    w(b, 0x04, (uint8_t)(rd(b, 0x04) | 1));
    w(b, 0x10, 1);
    clear_ints(b);
    ddc_abort(b);
}

static int hdcp_auth(const struct tx_bus *b)
{
    uint8_t ksv[5], an[8];
    int ones = 0, ok = 0;
    if (!(rd(b, 0x0e) & 0x10))
        return 0;
    w(b, 0x04, (uint8_t)(rd(b, 0x04) | 1));
    w(b, 0x20, 0);
    w(b, 0x22, 0);
    w(b, 0x10, 1);
    w(b, 0x15, 9);
    ddc_abort(b);
    w(b, 0x0f, 0);
    for (int i = 0; i < 40; i++) {
        b->sleep_ms(b->ctx, 15);
        if (hdcp_read(b, 0x40, 3))
            return 0;
        if ((rd(b, 0xc0) & 1) == (rd(b, 0x45) >> 4 & 1))
            break;
    }
    if (hdcp_read(b, 0, 5) || b->read_n(b->ctx, 0x3b, ksv, 5) < 0)
        return 0;
    for (int i = 0; i < 5; i++)
        for (int bit = 0; bit < 8; bit++)
            ones += ksv[i] >> bit & 1;
    if (ones != HDCP_KSV_ONES)
        return 0;
    w(b, 0x04, (uint8_t)(rd(b, 0x04) & ~1));
    w(b, 0x1f, 1);
    b->sleep_ms(b->ctx, 1);
    w(b, 0x1f, 0);
    if (b->read_n(b->ctx, 0x30, an, 8) < 0)
        return 0;
    for (uint8_t i = 0; i < 8; i++)
        w(b, (uint8_t)(0x28 + i), an[i]);
    w(b, 0x22, 0);
    w(b, 0x20, 1);
    clear_ints(b);
    w(b, 0x10, 1);
    w(b, 0x15, 9);
    w(b, 0x10, 0);
    w(b, 0x21, 1);
    for (int i = 0; i < 100; i++) {
        b->sleep_ms(b->ctx, 5);
        if (rd(b, 0x46) & 0x80) {
            ok = 1;
            break;
        }
        if (rd(b, 0x07) & 1) {
            w(b, 0x0c, 4);
            w(b, 0x0d, 0);
            w(b, 0x0e, 1);
            w(b, 0x0e, 0);
            break;
        }
    }
    return ok;
}

static void hdcp_sync(struct tx *t, const struct tx_bus *b)
{
    int want = t->state == TX_RUNNING && t->want && t->sink.hdmi && t->src_hdcp;
    if (t->hdcp_retry_polls_left)
        t->hdcp_retry_polls_left--;
    enum tx_hdcp_op op = tx_hdcp_next(t->hdcp, want, t->hdcp_retry_polls_left);
    if (op == HDCP_STOP)
        t->hdcp = HDCP_OFF;
    if (op == HDCP_AUTH) {
        mute(b);
        if (hdcp_auth(b)) {
            t->hdcp = HDCP_ON;
            unmute(b);
        } else {
            hdcp_stop(b);
            t->hdcp = HDCP_BACKOFF;
            t->hdcp_retry_polls_left = HDCP_BACKOFF_POLLS;
        }
    } else if (op == HDCP_STOP) {
        hdcp_stop(b);
        if (t->state == TX_RUNNING && t->want && !muted_pending_hdcp(t))
            unmute(b);
    }
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

static void enable_output(struct tx *t, const struct tx_bus *b)
{
    mute(b);
    w(b, 0x0f, 0);
    w(b, 0x61, 0x00);
    audio_sync(t, b);
    if (!muted_pending_hdcp(t))
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
    case TX_ENABLE: enable_output(t, b); break;
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

void tx_poll(struct tx *t, const struct tx_bus *b, const struct tx_mode *mode, int audio_hz, int src_hdcp)
{
    if (!t->present)
        return;
    if (rd(b, 0x09) != INIT_MARK) {
        chip_init(b);
        mute(b);
        t->audio_hz = 0;
        t->state = TX_IDLE;
        t->sent_vic = 0;
        t->hdcp = HDCP_OFF;
        t->hdcp_retry_polls_left = 0;
    }
    t->want = mode;
    t->want_hz = audio_hz;
    t->src_hdcp = src_hdcp;
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
    hdcp_sync(t, b);
    if (t->state == TX_RUNNING && mode && audio_wanted(t) != t->audio_hz) {
        mute(b);
        audio_sync(t, b);
        if (!muted_pending_hdcp(t))
            unmute(b);
    }
}

void tx_blank(struct tx *t, const struct tx_bus *b)
{
    if (t->present) {
        mute(b);
        if (t->hdcp != HDCP_OFF)
            hdcp_stop(b);
    }
    t->hdcp = HDCP_OFF;
    t->hdcp_retry_polls_left = 0;
    t->sent_vic = 0;
}
