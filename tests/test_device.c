#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/device.h"

struct fake {
    uint8_t mst[256][256];
    uint8_t codec[256];
    uint8_t bank;
    uint8_t pend_addr, pend_sub[8];
    int pend_len, pend_rl;
    int requests, fail_at, slept_ms;
    uint8_t first_req[3];
};

static void store(struct fake *f, uint8_t addr, const uint8_t *w, int n)
{
    if (addr == 0x94 && n >= 2)
        f->codec[w[0]] = w[1];
    if (addr != 0x9c || n < 2)
        return;
    if (w[0] == 0)
        f->bank = w[1];
    else
        f->mst[f->bank][w[0]] = w[1];
}

static int fake_ctrl(void *ctx, int in, uint8_t req, uint16_t val, uint16_t idx, uint8_t *buf, uint16_t len)
{
    (void)idx;
    struct fake *f = ctx;
    if (f->requests < 3)
        f->first_req[f->requests] = req;
    if (++f->requests == f->fail_at)
        return -7;
    if (req == 0xc1 && in) {
        buf[0] = 1;
        return 1;
    }
    if (req != 0xc0)
        return len;
    uint8_t addr = val & 0xff;
    if (!in && addr == 0x66) {
        if (buf[0] & 1) {
            f->pend_addr = buf[0] & 0xfe;
            f->pend_rl = buf[1];
            f->pend_len = len - 2;
            memcpy(f->pend_sub, buf + 2, (size_t)f->pend_len);
        } else {
            store(f, buf[0], buf + 1, len - 1);
        }
    } else if (!in) {
        store(f, addr, buf, len);
    } else {
        memset(buf, 0, len);
        if (f->pend_addr == 0xaa && f->pend_sub[2] == 0x57)
            buf[0] = 'Q', buf[1] = 0x10;
        else if (f->pend_addr == 0xaa && f->pend_sub[2] == 0x58)
            buf[0] = 1, buf[1] = 2, buf[2] = 3;
        else if (f->pend_addr == 0x94)
            buf[0] = f->codec[f->pend_sub[0]];
        else if (f->pend_addr == 0x9c)
            buf[0] = f->mst[f->bank][f->pend_sub[0]];
    }
    return len;
}

static void fake_sleep(void *ctx, int ms) { ((struct fake *)ctx)->slept_ms += ms; }

static struct fake fk;

static void boot(struct hd60s *d, int fail_at)
{
    memset(&fk, 0, sizeof fk);
    fk.fail_at = fail_at;
    memset(d, 0, sizeof *d);
    d->usb = (struct hd60s_usb){&fk, fake_ctrl, fake_sleep};
}

static void init_runs_vendor_sequence_and_reads_mcu_version(void)
{
    struct hd60s d;
    boot(&d, 0);
    assert(hd60s_init(&d) == 0);
    assert(fk.first_req[0] == 0xec && fk.first_req[1] == 0xc2 && fk.first_req[2] == 0xc7);
    assert(d.mcu_version == 0x010203);
    assert(hd60s_start(&d) == 0);
    assert(d.started && d.sd == 0);
    hd60s_stop(&d);
    assert(!d.started);
}

static void init_ignores_vendor_init_failures(void)
{
    struct hd60s d;
    boot(&d, 2);
    assert(hd60s_init(&d) == 0);
    assert(d.err == 0);
}

static void first_transport_error_fails_start(void)
{
    struct hd60s d;
    boot(&d, 0);
    assert(hd60s_init(&d) == 0);
    fk.fail_at = fk.requests + 5;
    assert(hd60s_start(&d) < 0);
    assert(d.err == -7 && !d.started);
}

static void policy_switches_colour_matrix_once_per_change(void)
{
    struct hd60s d;
    struct hd60s_parser p = {0};
    struct hd60s_policy pol;
    boot(&d, 0);
    assert(hd60s_init(&d) == 0 && hd60s_start(&d) == 0);
    hd60s_policy_begin(&pol, 0);
    p.last_w = 720, p.last_h = 576;
    hd60s_policy_tick(&d, &p, &pol, 1000);
    assert(d.sd == 1);
    int before = fk.requests;
    hd60s_policy_tick(&d, &p, &pol, 1100);
    assert(fk.requests == before);
    p.last_w = 1920, p.last_h = 1080;
    hd60s_policy_tick(&d, &p, &pol, 1150);
    assert(d.sd == 0 && fk.requests > before);
}

static void policy_reads_aspect_once_a_second(void)
{
    struct hd60s d;
    struct hd60s_parser p = {0};
    struct hd60s_policy pol;
    boot(&d, 0);
    assert(hd60s_init(&d) == 0 && hd60s_start(&d) == 0);
    fk.mst[2][0x0b] = 0x08;
    fk.mst[2][0x49] = 0x20;
    hd60s_policy_begin(&pol, 0);
    hd60s_policy_tick(&d, &p, &pol, 0);
    assert(p.wide == 1);
    fk.mst[2][0x49] = 0x10;
    hd60s_policy_tick(&d, &p, &pol, 500);
    assert(p.wide == 1);
    hd60s_policy_tick(&d, &p, &pol, 1000);
    assert(p.wide == 0);
}

static void policy_finds_mode_from_a_stable_window(void)
{
    struct hd60s_policy pol;
    hd60s_policy_begin(&pol, 0);
    for (int i = 0; i < 120; i++)
        hd60s_policy_frame(&pol, 1920, 1080, 0);
    struct hd60s d;
    struct hd60s_parser p = {0};
    boot(&d, 0);
    assert(hd60s_init(&d) == 0 && hd60s_start(&d) == 0);
    hd60s_policy_tick(&d, &p, &pol, 2000);
    assert(pol.mode && pol.mode->vic == 16);
}

static void start_routes_hdmi_audio_by_default(void)
{
    struct hd60s d;
    boot(&d, 0);
    assert(hd60s_init(&d) == 0 && hd60s_start(&d) == 0);
    assert(fk.codec[0x03] == 0xa1 && (fk.codec[0x04] & 0x43) == 0x40);
    assert(fk.codec[0x0e] & 0x80 && fk.codec[0x0f] & 0x80);
    assert(!(fk.codec[0x10] & 0x80) && !(fk.codec[0x11] & 0x80));
}

static void audio_settings_apply_at_start_and_live(void)
{
    struct hd60s d;
    boot(&d, 0);
    assert(hd60s_init(&d) == 0);
    struct audio_settings line = {AUDIO_LINE, -12, 0};
    assert(hd60s_set_audio(&d, &line) == 0);
    assert(fk.codec[0x03] == 0 && d.audio.source == AUDIO_LINE); /* nothing sent before start */
    assert(hd60s_start(&d) == 0);
    assert(!(fk.codec[0x0e] & 0x80) && fk.codec[0x10] & 0x80);
    assert(fk.codec[0x0c] == 0x74 && fk.codec[0x0a] == 0); /* HDMI-mode gain registers */
    struct audio_settings mute = {AUDIO_LINE, -12, 1};
    assert(hd60s_set_audio(&d, &mute) == 0);
    assert(fk.codec[0x0e] & 0x80 && fk.codec[0x0f] & 0x80);
    struct audio_settings out_of_range = {AUDIO_MIX, 99, 0};
    assert(hd60s_set_audio(&d, &out_of_range) == 0);
    assert(d.audio.gain_half_db == AUDIO_GAIN_MAX);
}

static void policy_reruns_routing_when_the_source_becomes_hdmi(void)
{
    struct hd60s d;
    struct hd60s_parser p = {0};
    struct hd60s_policy pol;
    boot(&d, 0);
    assert(hd60s_init(&d) == 0 && hd60s_start(&d) == 0);
    hd60s_policy_begin(&pol, 0);
    hd60s_policy_tick(&d, &p, &pol, 0);
    assert(fk.codec[0x03] == 0xa1);
    int before = fk.requests;
    hd60s_policy_tick(&d, &p, &pol, 1000);
    int idle_cost = fk.requests - before; /* polls only, no routing writes */
    fk.mst[0][0x51] = 0x80; /* receiver sees a source ... */
    fk.mst[1][0x01] = 0x04; /* ... in the DVI branch */
    hd60s_policy_tick(&d, &p, &pol, 2000);
    assert(fk.codec[0x03] == 0xa0 && fk.mst[2][0x27] == 0);
    before = fk.requests;
    hd60s_policy_tick(&d, &p, &pol, 3000);
    assert(fk.requests - before == idle_cost);
}

int main(void)
{
    init_runs_vendor_sequence_and_reads_mcu_version();
    init_ignores_vendor_init_failures();
    first_transport_error_fails_start();
    policy_switches_colour_matrix_once_per_change();
    policy_reads_aspect_once_a_second();
    policy_finds_mode_from_a_stable_window();
    start_routes_hdmi_audio_by_default();
    audio_settings_apply_at_start_and_live();
    policy_reruns_routing_when_the_source_becomes_hdmi();
    puts("test_device: ok");
    return 0;
}
