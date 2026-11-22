#include "../src/tx.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Fake IT66121: two register banks, level inputs, DDC FIFO fed from an EDID image. */
struct fake {
    uint8_t reg[2][256];
    int bank, hpd, stable, present, ddc_fail, writes, rejected;
    uint8_t edid[640];
    uint8_t fifo[32];
    uint8_t wlog[4096][3]; /* reg, val, bank in effect */
    int nlog;
};

static int f_read(void *c, uint8_t r)
{
    struct fake *f = c;
    if (r == 0x01) return f->present ? 0x49 : 0xff;
    if (r == 0x02) return f->present ? 0x12 : 0xff;
    if (r == 0x03) return f->present ? 0x16 : 0xff;
    if (r == 0x0e) return (f->hpd ? 0x40 : 0) | (f->stable ? 0x10 : 0);
    if (r == 0x16) return f->ddc_fail ? 0x20 : 0x80;
    return f->reg[f->bank][r];
}

static void f_write(void *c, uint8_t r, uint8_t v)
{
    struct fake *f = c;
    f->wlog[f->nlog][0] = r;
    f->wlog[f->nlog][1] = v;
    f->wlog[f->nlog++][2] = (uint8_t)f->bank;
    if (r == 0x0f) f->bank = v & 1;
    f->reg[f->bank][r] = v;
    if (r == 0x15 && v == 0x03) {
        int seg = f->reg[0][0x14], off = f->reg[0][0x12], n = f->reg[0][0x13];
        memcpy(f->fifo, f->edid + seg * 256 + off, (size_t)n);
    }
}

static int f_read_n(void *c, uint8_t r, uint8_t *b, int n)
{
    struct fake *f = c;
    assert(r == 0x17);
    memcpy(b, f->fifo, (size_t)n);
    return 0;
}

static void f_sleep(void *c, int ms) { (void)c; (void)ms; }

static struct tx_bus bus_for(struct fake *f) { return (struct tx_bus){f, f_read, f_write, f_read_n, f_sleep}; }

static void csum(uint8_t *b)
{
    unsigned s = 0;
    for (int i = 0; i < 127; i++) s += b[i];
    b[127] = (uint8_t)(0x100 - (s & 0xff));
}

static void make_edid(uint8_t *e, int hdmi, int ycc444, int ycc422)
{
    memset(e, 0, 256);
    memcpy(e, "\x00\xff\xff\xff\xff\xff\xff\x00", 8);
    e[0x14] = 0x80;
    e[0x7e] = 1;
    csum(e);
    uint8_t *x = e + 128;
    x[0] = 2;
    x[1] = 3;
    x[2] = hdmi ? 10 : 4;
    x[3] = (uint8_t)((ycc444 ? 0x20 : 0) | (ycc422 ? 0x10 : 0) | 0x40);
    if (hdmi) {
        x[4] = (3 << 5) | 5;
        x[5] = 0x03; x[6] = 0x0c; x[7] = 0x00; x[8] = 0x10; x[9] = 0x00;
    }
    csum(x);
}

static void tv_no_audio(struct fake *f)
{
    f->edid[128 + 3] &= (uint8_t)~0x40;
    csum(f->edid + 128);
}

static void tv(struct fake *f, int hdmi, int c444, int c422)
{
    make_edid(f->edid, hdmi, c444, c422);
    f->hpd = 1;
}

static void setup(struct fake *f, struct tx *t, struct tx_bus *b)
{
    memset(f, 0, sizeof *f);
    f->present = 1;
    f->stable = 1;
    f->reg[0][0x09] = 0xb2;
    *b = bus_for(f);
    memset(t, 0, sizeof *t);
}

static const struct tx_mode *m1080p60(void) { return tx_mode_find(1920, 1080, 0, 60.0); }

static int saw(const struct fake *f, uint8_t r, uint8_t v)
{
    for (int i = 0; i < f->nlog; i++)
        if (f->wlog[i][0] == r && f->wlog[i][1] == v) return 1;
    return 0;
}

static void test_modes(void)
{
    assert(m1080p60()->vic == 16 && m1080p60()->wide);
    assert(tx_mode_find(1920, 1080, 0, 50.2)->vic == 31);
    assert(tx_mode_find(1920, 1080, 1, 59.9)->vic == 5);
    assert(tx_mode_find(1920, 1080, 1, 50.0)->vic == 20);
    assert(tx_mode_find(1280, 720, 0, 59.94)->vic == 4);
    assert(tx_mode_find(1280, 720, 0, 50.0)->vic == 19);
    assert(tx_mode_find(720, 576, 1, 50.0)->vic == 21 && tx_mode_find(720, 576, 1, 50.0)->rep == 1);
    assert(tx_mode_find(720, 480, 0, 60.0)->vic == 2 && !tx_mode_find(720, 480, 0, 60.0)->wide);
    assert(tx_mode_find(640, 480, 0, 60.0)->vic == 1);
    assert(tx_mode_find(1920, 1080, 0, 24.0)->vic == 32);
    assert(tx_mode_find(1920, 1080, 0, 30.0)->vic == 34);
    assert(!tx_mode_find(1920, 1080, 0, 45.0));
    assert(!tx_mode_find(1440, 480, 1, 60.0));
    assert(!tx_mode_find(1600, 900, 0, 60.0));
}

static void test_edid(void)
{
    uint8_t e[256];
    struct tx_sink s;
    make_edid(e, 1, 1, 1);
    assert(tx_edid_parse(e, 256, &s) && s.valid && s.hdmi && s.ycc444 && s.ycc422 && s.basic_audio);
    e[128 + 3] &= (uint8_t)~0x40;
    csum(e + 128);
    assert(tx_edid_parse(e, 256, &s) && s.hdmi && !s.basic_audio);
    make_edid(e, 0, 0, 0);
    assert(tx_edid_parse(e, 256, &s) && s.valid && !s.hdmi && !s.ycc444);
    make_edid(e, 1, 0, 1);
    assert(tx_edid_parse(e, 256, &s) && s.hdmi && !s.ycc444 && s.ycc422);
    make_edid(e, 1, 1, 1);
    e[10] ^= 1;
    assert(!tx_edid_parse(e, 256, &s));
    make_edid(e, 1, 1, 1);
    e[0] = 1;
    assert(!tx_edid_parse(e, 256, &s));
    make_edid(e, 1, 1, 1);
    e[128 + 5] = 0;
    csum(e + 128);
    assert(tx_edid_parse(e, 256, &s) && s.valid && !s.hdmi);
}

static void test_audio_tables(void)
{
    assert(tx_audio_n(27000, 48000) == 6144 && tx_audio_n(74250, 48000) == 6144 && tx_audio_n(148500, 48000) == 6144);
    assert(tx_audio_n(27000, 44100) == 6272 && tx_audio_n(74250, 44100) == 6272 && tx_audio_n(148500, 44100) == 6272);
    assert(tx_audio_n(27000, 32000) == 4096 && tx_audio_n(74250, 32000) == 4096 && tx_audio_n(148500, 32000) == 4096);
    assert(tx_audio_n(25175, 32000) == 4576 && tx_audio_n(25175, 44100) == 7007 && tx_audio_n(25175, 48000) == 6864);
    assert(tx_audio_n(74250, 96000) == 0 && tx_audio_n(74250, 0) == 0);
    assert(tx_audio_fs_code(44100) == 0 && tx_audio_fs_code(48000) == 2 && tx_audio_fs_code(32000) == 3);
    /* fs = pclk * N / (128 * CTS), snapped to a standard rate */
    assert(tx_audio_rate(74250, 6144, 74250) == 48000);
    assert(tx_audio_rate(148500, 6272, 165000) == 44100);
    assert(tx_audio_rate(27000, 4096, 27000) == 32000);
    assert(tx_audio_rate(25175, 6864, 28125) == 48000);
    assert(tx_audio_rate(74250, 6144, 74176) == 48000); /* 1001 pixel clock, same N */
    assert(tx_audio_rate(74250, 12288, 74250) == 96000);
    assert(tx_audio_rate(74250, 0, 74250) == 0 && tx_audio_rate(74250, 6144, 0) == 0 && tx_audio_rate(74250, 5000, 74250) == 0);
}

static void test_avi(void)
{
    uint8_t a[6];
    tx_avi(a, TX_YCC444, m1080p60());
    assert(!memcmp(a, "\x50\xa8\x00\x10\x00\x67", 6));
    tx_avi(a, TX_RGB, tx_mode_find(720, 576, 1, 50.0));
    assert(a[0] == 0x10 && a[1] == 0x58 && a[3] == 21 && a[4] == 1);
    assert((uint8_t)(a[0] + a[1] + a[2] + a[3] + a[4] + a[5] + 0x82 + 2 + 13) == 0);
    /* colorimetry C1C0: 01 = BT.601 for SD rows, 10 = BT.709 for HD rows */
    assert(a[1] >> 6 == 1);
    tx_avi(a, TX_RGB, tx_mode_find(720, 480, 0, 59.94));
    assert(a[1] == 0x58 && a[3] == 2);
    tx_avi(a, TX_RGB, tx_mode_find(640, 480, 0, 60.0));
    assert(a[1] >> 6 == 1);
    tx_avi(a, TX_RGB, tx_mode_find(1280, 720, 0, 60.0));
    assert(a[1] >> 6 == 2);
    tx_avi(a, TX_RGB, tx_mode_find(1920, 1080, 1, 60.0));
    assert(a[1] >> 6 == 2);
}

static void test_transitions(void)
{
    struct tx_in on = {.hpd = 1, .stable = 1};
    assert(tx_next(TX_IDLE, on) == TX_EDID);
    assert(tx_next(TX_IDLE, (struct tx_in){0}) == TX_IDLE);
    assert(tx_next(TX_EDID, on) == TX_IDLE);
    on.edid_ok = 1;
    assert(tx_next(TX_EDID, on) == TX_PICK);
    assert(tx_next(TX_PICK, on) == TX_VIDEO);
    assert(tx_next(TX_VIDEO, on) == TX_ENABLE);
    assert(tx_next(TX_VIDEO, (struct tx_in){.hpd = 1}) == TX_UNSTABLE);
    assert(tx_next(TX_UNSTABLE, (struct tx_in){.hpd = 1}) == TX_UNSTABLE);
    assert(tx_next(TX_UNSTABLE, on) == TX_ENABLE);
    assert(tx_next(TX_UNSTABLE, (struct tx_in){.hpd = 1, .stable = 1, .mode_changed = 1}) == TX_VIDEO);
    assert(tx_next(TX_ENABLE, on) == TX_RUNNING);
    assert(tx_next(TX_RUNNING, on) == TX_RUNNING);
    assert(tx_next(TX_RUNNING, (struct tx_in){.hpd = 1, .stable = 1, .mode_changed = 1}) == TX_VIDEO);
    assert(tx_next(TX_RUNNING, (struct tx_in){.hpd = 1, .mode_changed = 1}) == TX_UNSTABLE);
    assert(tx_next(TX_RUNNING, (struct tx_in){.hpd = 1, .stable = 1, .blank = 1, .mode_changed = 1}) == TX_RUNNING);
    assert(tx_next(TX_PICK, (struct tx_in){.hpd = 1, .stable = 1, .blank = 1}) == TX_PICK);
    for (int s = TX_IDLE; s <= TX_RUNNING; s++)
        assert(tx_next((enum tx_state)s, (struct tx_in){.stable = 1, .edid_ok = 1}) == TX_IDLE);
}

static void test_write_guard(void)
{
    struct fake f;
    struct tx t;
    struct tx_bus b;
    setup(&f, &t, &b);
    assert(!tx_writable(0x01, 0));
    assert(!tx_writable(0x20, 1));
    assert(!tx_writable(0x21, 1));
    assert(!tx_writable(0x1f, 1));
    assert(!tx_writable(0xe0, 0x55) && !tx_writable(0xe5, 0) && !tx_writable(0xc5, 1) && !tx_writable(0xce, 2));
    assert(tx_writable(0xe0, 0xc1) && tx_writable(0xe0, 0) && tx_writable(0xc5, 0) && tx_writable(0xce, 3));
    assert(tx_writable(0x33, 0x18) && tx_writable(0x6d, 0x70));
    assert(!tx_writable(0x15, 0x01));
    assert(!tx_writable(0x15, 0x05));
    assert(!tx_writable(0x11, 0x74));
    assert(!tx_writable(0x11, 0xa1));
    int n = f.nlog;
    assert(tx_write(&b, 0x20, 1) < 0 && tx_write(&b, 0x15, 1) < 0 && tx_write(&b, 0x01, 0) < 0 && tx_write(&b, 0x10, 0) < 0);
    assert(f.nlog == n);
    assert(tx_write(&b, 0x61, 0x30) == 0 && f.nlog == n + 1);
    assert(tx_writable(0x15, 0x03) && tx_writable(0x15, 0x09) && tx_writable(0x15, 0x0f) && tx_writable(0x15, 0));
    assert(tx_writable(0x11, 0xa0) && tx_writable(0x61, 0) && tx_writable(0xc1, 1));
}

static void check_only_allowed(const struct fake *f)
{
    for (int i = 0; i < f->nlog; i++) {
        unsigned r = f->wlog[i][0], v = f->wlog[i][1];
        int ok = (r >= 0x04 && r <= 0x05) || (r >= 0x09 && r <= 0x0b) || r == 0x0f || (r >= 0x10 && r <= 0x14) ||
                 r == 0x15 || (r >= 0x58 && r <= 0x68) || (r >= 0x70 && r <= 0x87) || (r >= 0x90 && r <= 0xa3) ||
                 r == 0xc0 || r == 0xc1 || r == 0xc6 || (r >= 0xc9 && r <= 0xd0);
        unsigned bk = f->wlog[i][2];
        /* audio: N, InfoFrame and channel status live in bank 1 only; input config, CTS and packet enable in bank 0 */
        if (r >= 0x33 && r <= 0x35) ok = bk == 1;
        if (r >= 0x69 && r <= 0x6d) ok = bk == 1;
        if (r == 0xc5) ok = v == 0;
        if (r == 0xce) ok = (v == 0 || v == 3) && bk == 0;
        if (r >= 0xe0 && r <= 0xe4) ok = bk == 0 && v == (const uint8_t[]){0xc1, 0x01, 0xe4, 0x00, 0x08}[r - 0xe0] ;
        if (r == 0xe0 && v == 0) ok = 1;
        if (!ok) fprintf(stderr, "bad write %02x=%02x\n", r, v);
        assert(ok);
        if (r == 0x15) assert(v == 0 || v == 3 || v == 9 || v == 0xf);
        if (r == 0x11) assert(v == 0xa0);
        if (r == 0x10) assert(v == 1);
        if (r == 0x65) assert(v == 0 || v == 2);
    }
}

static void test_start_with_tv(void)
{
    struct fake f;
    struct tx t;
    struct tx_bus b;
    setup(&f, &t, &b);
    tv(&f, 1, 1, 1);
    tx_start(&t, &b);
    assert(t.present);
    tx_poll(&t, &b, m1080p60(), 0);
    assert(t.state == TX_RUNNING && t.colour == TX_YCC444);
    assert(!memcmp(&f.reg[1][0x58], "\x50\xa8\x00\x10\x00\x67", 6));
    assert(f.reg[0][0x61] == 0x00 && f.reg[0][0xc1] == 0x00 && f.reg[0][0xc0] == 1);
    assert((f.reg[0][0x70] & 0xdc) == 0x40);
    assert(f.reg[0][0xcd] == 0x03);
    assert(f.reg[0][0x0f] == 0);
    assert(!saw(&f, 0x04, 0x3d));
    check_only_allowed(&f);
}

static void test_no_tv_then_hotplug(void)
{
    struct fake f;
    struct tx t;
    struct tx_bus b;
    setup(&f, &t, &b);
    tx_start(&t, &b);
    tx_poll(&t, &b, m1080p60(), 0);
    assert(t.state == TX_IDLE && f.nlog == 0);
    tv(&f, 1, 0, 1);
    tx_poll(&t, &b, m1080p60(), 0);
    assert(t.state == TX_RUNNING && t.colour == TX_YCC422);
    assert(f.reg[1][0x58] == 0x30);
    check_only_allowed(&f);
}

static void test_mode_change(void)
{
    struct fake f;
    struct tx t;
    struct tx_bus b;
    setup(&f, &t, &b);
    tv(&f, 1, 1, 1);
    tx_start(&t, &b);
    tx_poll(&t, &b, m1080p60(), 0);
    f.nlog = 0;
    tx_poll(&t, &b, tx_mode_find(1280, 720, 0, 50.0), 0);
    assert(t.state == TX_RUNNING && f.reg[1][0x5b] == 19);
    assert(saw(&f, 0xc1, 1) && f.reg[0][0xc1] == 0);
    assert(f.reg[0][0x62] & 0x10);
    f.nlog = 0;
    tx_poll(&t, &b, tx_mode_find(1280, 720, 0, 50.0), 0);
    assert(f.nlog == 0);
    tx_poll(&t, &b, m1080p60(), 0);
    assert(f.reg[1][0x5b] == 16 && (f.reg[0][0x62] & 0x80));
    check_only_allowed(&f);
}

static int idx(const struct fake *f, uint8_t r, int v, int from)
{
    for (int i = from; i < f->nlog; i++)
        if (f->wlog[i][0] == r && (v < 0 || f->wlog[i][1] == v)) return i;
    return -1;
}

static int last(const struct fake *f, uint8_t r, int v)
{
    for (int i = f->nlog - 1; i >= 0; i--)
        if (f->wlog[i][0] == r && f->wlog[i][1] == v) return i;
    return -1;
}

static int audio_touched(const struct fake *f)
{
    for (int i = 0; i < f->nlog; i++) {
        unsigned r = f->wlog[i][0];
        if ((r >= 0xe0 && r <= 0xe4) || r == 0xce || r == 0xc5 || (r >= 0x33 && r <= 0x35)) return 1;
    }
    return 0;
}

static void test_audio_48k(void)
{
    struct fake f;
    struct tx t;
    struct tx_bus b;
    setup(&f, &t, &b);
    tv(&f, 1, 1, 1);
    tx_start(&t, &b);
    tx_poll(&t, &b, m1080p60(), 48000);
    assert(t.state == TX_RUNNING && t.audio_hz == 48000);
    assert(!memcmp(&f.reg[1][0x33], "\x00\x18\x00", 3));
    assert(f.reg[0][0xc5] == 0 && (f.reg[0][0x58] & 0x0c) == 0x04);
    assert(!memcmp(&f.reg[0][0xe0], "\xc1\x01\xe4\x00\x08", 5));
    assert(!memcmp(&f.reg[1][0x91], "\x00\x00\x01\x21\x43\x65\x87\x02\xdb", 9));
    assert(f.reg[1][0x68] == 0x01 && f.reg[1][0x6b] == 0 && f.reg[1][0x6c] == 0 && f.reg[1][0x6d] == 0x70);
    assert(f.reg[0][0xce] == 0x03 && !(f.reg[0][0x04] & 0x04));
    assert(f.reg[0][0xc1] == 0x00 && f.reg[0][0x61] == 0x00);
    /* audio is set up while muted: before the final unmute */
    assert(idx(&f, 0xce, 3, 0) < last(&f, 0xc1, 0));
    check_only_allowed(&f);
}

static void test_audio_rates_and_clocks(void)
{
    struct fake f;
    struct tx t;
    struct tx_bus b;
    setup(&f, &t, &b);
    tv(&f, 1, 1, 1);
    tx_start(&t, &b);
    tx_poll(&t, &b, tx_mode_find(640, 480, 0, 60.0), 44100);
    assert(!memcmp(&f.reg[1][0x33], "\x5f\x1b\x00", 3) && f.reg[1][0x98] == 0x00 && f.reg[1][0x99] == 0xfb);
    tx_poll(&t, &b, tx_mode_find(1280, 720, 0, 50.0), 44100);
    assert(!memcmp(&f.reg[1][0x33], "\x80\x18\x00", 3) && t.audio_hz == 44100);
    tx_poll(&t, &b, tx_mode_find(1280, 720, 0, 50.0), 32000);
    assert(!memcmp(&f.reg[1][0x33], "\x00\x10\x00", 3) && f.reg[1][0x98] == 0x03 && f.reg[1][0x99] == 0xcb);
    check_only_allowed(&f);
}

static void test_audio_rate_change_is_muted(void)
{
    struct fake f;
    struct tx t;
    struct tx_bus b;
    setup(&f, &t, &b);
    tv(&f, 1, 1, 1);
    tx_start(&t, &b);
    tx_poll(&t, &b, m1080p60(), 48000);
    f.nlog = 0;
    tx_poll(&t, &b, m1080p60(), 48000);
    assert(f.nlog == 0);
    tx_poll(&t, &b, m1080p60(), 44100);
    int mute = idx(&f, 0xc1, 1, 0), n = idx(&f, 0x33, -1, 0), un = last(&f, 0xc1, 0);
    assert(mute >= 0 && mute < n && n < un && f.reg[0][0xc1] == 0);
    assert(!memcmp(&f.reg[1][0x33], "\x80\x18\x00", 3) && t.state == TX_RUNNING);
    check_only_allowed(&f);
}

static void test_audio_source_lost_and_back(void)
{
    struct fake f;
    struct tx t;
    struct tx_bus b;
    setup(&f, &t, &b);
    tv(&f, 1, 1, 1);
    tx_start(&t, &b);
    tx_poll(&t, &b, m1080p60(), 48000);
    tx_poll(&t, &b, m1080p60(), 0);
    assert(t.audio_hz == 0 && f.reg[0][0xce] == 0 && f.reg[0][0xe0] == 0 && t.state == TX_RUNNING);
    assert(f.reg[0][0xc1] == 0);
    f.nlog = 0;
    tx_poll(&t, &b, m1080p60(), 0);
    assert(f.nlog == 0);
    tx_poll(&t, &b, m1080p60(), 48000);
    assert(t.audio_hz == 48000 && f.reg[0][0xce] == 3 && f.reg[0][0xe0] == 0xc1);
    check_only_allowed(&f);
}

static void test_audio_off_cases(void)
{
    struct fake f;
    struct tx t;
    struct tx_bus b;
    setup(&f, &t, &b);
    tv(&f, 1, 1, 1);
    tv_no_audio(&f);
    tx_start(&t, &b);
    tx_poll(&t, &b, m1080p60(), 48000);
    assert(t.state == TX_RUNNING && t.audio_hz == 0 && !audio_touched(&f));
    setup(&f, &t, &b);
    tv(&f, 0, 0, 0);
    tx_start(&t, &b);
    tx_poll(&t, &b, m1080p60(), 48000);
    assert(t.state == TX_RUNNING && t.audio_hz == 0 && !audio_touched(&f));
    setup(&f, &t, &b);
    tv(&f, 1, 1, 1);
    tx_start(&t, &b);
    tx_poll(&t, &b, m1080p60(), 96000);
    assert(t.state == TX_RUNNING && t.audio_hz == 0 && !audio_touched(&f));
    tx_poll(&t, &b, m1080p60(), 48000);
    assert(t.audio_hz == 48000);
    tx_poll(&t, &b, m1080p60(), 96000);
    assert(t.audio_hz == 0 && f.reg[0][0xce] == 0);
    check_only_allowed(&f);
}

static void test_audio_unplug_replug(void)
{
    struct fake f;
    struct tx t;
    struct tx_bus b;
    setup(&f, &t, &b);
    tv(&f, 1, 1, 1);
    tx_start(&t, &b);
    tx_poll(&t, &b, m1080p60(), 48000);
    f.hpd = 0;
    tx_poll(&t, &b, m1080p60(), 48000);
    assert(t.state == TX_IDLE && t.audio_hz == 0 && f.reg[0][0xce] == 0);
    tv(&f, 1, 1, 1);
    tv_no_audio(&f);
    f.nlog = 0;
    tx_poll(&t, &b, m1080p60(), 48000);
    assert(t.state == TX_RUNNING && t.audio_hz == 0 && !audio_touched(&f));
    f.hpd = 0;
    tx_poll(&t, &b, m1080p60(), 48000);
    tv(&f, 1, 1, 1);
    f.nlog = 0;
    tx_poll(&t, &b, m1080p60(), 48000);
    assert(t.audio_hz == 48000 && f.reg[0][0xce] == 3);
    check_only_allowed(&f);
}

static void test_blank_and_resume(void)
{
    struct fake f;
    struct tx t;
    struct tx_bus b;
    setup(&f, &t, &b);
    tv(&f, 1, 1, 1);
    tx_start(&t, &b);
    tx_poll(&t, &b, m1080p60(), 0);
    tx_poll(&t, &b, NULL, 0);
    assert(t.state == TX_RUNNING && f.reg[0][0xc1] == 1 && f.reg[0][0xc6] == 3);
    tx_poll(&t, &b, m1080p60(), 0);
    assert(t.state == TX_RUNNING && f.reg[0][0xc1] == 0);
    tx_blank(&t, &b);
    assert(f.reg[0][0xc1] == 1);
}

static void test_unplug_replug(void)
{
    struct fake f;
    struct tx t;
    struct tx_bus b;
    setup(&f, &t, &b);
    tv(&f, 1, 1, 1);
    tx_start(&t, &b);
    tx_poll(&t, &b, m1080p60(), 0);
    f.hpd = 0;
    tx_poll(&t, &b, m1080p60(), 0);
    assert(t.state == TX_IDLE && f.reg[0][0x61] == 0x30);
    tv(&f, 0, 0, 0);
    tx_poll(&t, &b, m1080p60(), 0);
    assert(t.state == TX_RUNNING && t.colour == TX_RGB && f.reg[0][0xc0] == 0);
    assert(f.reg[0][0x72] == 0x03 && f.reg[0][0x73] == 0x10 && f.reg[0][0x76] == 0x4f);
    check_only_allowed(&f);
}

static void test_unstable_source(void)
{
    struct fake f;
    struct tx t;
    struct tx_bus b;
    setup(&f, &t, &b);
    tv(&f, 1, 1, 1);
    f.stable = 0;
    tx_start(&t, &b);
    tx_poll(&t, &b, m1080p60(), 0);
    assert(t.state == TX_UNSTABLE && f.reg[0][0x61] != 0);
    f.stable = 1;
    tx_poll(&t, &b, m1080p60(), 0);
    assert(t.state == TX_RUNNING);
    f.stable = 0;
    tx_poll(&t, &b, m1080p60(), 0);
    assert(t.state == TX_UNSTABLE && f.reg[0][0xc1] == 1);
    f.nlog = 0;
    f.stable = 1;
    tx_poll(&t, &b, m1080p60(), 0);
    assert(t.state == TX_RUNNING && f.reg[0][0xc1] == 0 && !saw(&f, 0x04, 0x1d));
}

static void test_edid_failure_backs_off(void)
{
    struct fake f;
    struct tx t;
    struct tx_bus b;
    setup(&f, &t, &b);
    tv(&f, 1, 1, 1);
    f.ddc_fail = 1;
    tx_start(&t, &b);
    tx_poll(&t, &b, m1080p60(), 0);
    assert(t.state == TX_IDLE);
    int n = f.nlog;
    tx_poll(&t, &b, m1080p60(), 0);
    assert(f.nlog == n);
    f.ddc_fail = 0;
    for (int i = 0; i < 20 && t.state != TX_RUNNING; i++) tx_poll(&t, &b, m1080p60(), 0);
    assert(t.state == TX_RUNNING);
    check_only_allowed(&f);
}

static void test_init_when_lost(void)
{
    struct fake f;
    struct tx t;
    struct tx_bus b;
    setup(&f, &t, &b);
    f.reg[0][0x09] = 0;
    tx_start(&t, &b);
    assert(saw(&f, 0x04, 0x3d) && saw(&f, 0x04, 0x1d) && f.reg[0][0x09] == 0xb2 && f.reg[0][0x61] == 0x30);
    assert(f.reg[0][0x0a] == 0xf8 && f.reg[0][0x0b] == 0x37);
    f.reg[0][0x09] = 0;
    f.nlog = 0;
    tx_poll(&t, &b, m1080p60(), 0);
    assert(saw(&f, 0x04, 0x3d));
    check_only_allowed(&f);
}

static void test_absent(void)
{
    struct fake f;
    struct tx t;
    struct tx_bus b;
    setup(&f, &t, &b);
    f.present = 0;
    tv(&f, 1, 1, 1);
    tx_start(&t, &b);
    tx_poll(&t, &b, m1080p60(), 0);
    tx_blank(&t, &b);
    assert(!t.present && f.nlog == 0);
}

int main(void)
{
    test_modes();
    test_edid();
    test_audio_tables();
    test_avi();
    test_transitions();
    test_write_guard();
    test_start_with_tv();
    test_no_tv_then_hotplug();
    test_mode_change();
    test_audio_48k();
    test_audio_rates_and_clocks();
    test_audio_rate_change_is_muted();
    test_audio_source_lost_and_back();
    test_audio_off_cases();
    test_audio_unplug_replug();
    test_blank_and_resume();
    test_unplug_replug();
    test_unstable_source();
    test_edid_failure_backs_off();
    test_init_when_lost();
    test_absent();
    puts("test_tx ok");
    return 0;
}
