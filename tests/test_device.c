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
    d->usb = (struct hd60s_usb){.ctx = &fk, .ctrl = fake_ctrl, .sleep_ms = fake_sleep};
}

static void init_runs_vendor_sequence_and_reads_mcu_version(void)
{
    struct hd60s d;
    boot(&d, 0);
    assert(hd60s_init(&d) == 0);
    assert(fk.first_req[0] == 0xec && fk.first_req[1] == 0xc2 && fk.first_req[2] == 0xc7);
    assert(d.mcu_version == 0x010203);
    assert(hd60s_start(&d) == 0);
    assert(d.started && d.colour.sd == 0 && d.colour.input == INPUT_RGB);
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
    assert(d.colour.sd == 1);
    int before = fk.requests;
    hd60s_policy_tick(&d, &p, &pol, 1100);
    assert(fk.requests == before);
    p.last_w = 1920, p.last_h = 1080;
    hd60s_policy_tick(&d, &p, &pol, 1150);
    assert(d.colour.sd == 0 && fk.requests > before);
}

static void avi_parse_covers_each_format_and_colorimetry(void)
{
    struct hd60s_source s = hd60s_parse_avi(1, 0x00, 0x00);
    assert(s.format == INPUT_RGB && s.colorimetry == COLOUR_UNKNOWN);
    s = hd60s_parse_avi(1, 0x20, 0x40);
    assert(s.format == INPUT_YCBCR422 && s.colorimetry == COLOUR_BT601);
    s = hd60s_parse_avi(1, 0x40, 0x80);
    assert(s.format == INPUT_YCBCR444 && s.colorimetry == COLOUR_BT709);
    s = hd60s_parse_avi(1, 0x40, 0xc0);
    assert(s.format == INPUT_YCBCR444 && s.colorimetry == COLOUR_UNKNOWN);
    s = hd60s_parse_avi(1, 0x60, 0x80); /* reserved Y1Y0: Elgato treats it as RGB */
    assert(s.format == INPUT_RGB);
    s = hd60s_parse_avi(1, 0x1f, 0x3f); /* other bits ignored */
    assert(s.format == INPUT_RGB && s.colorimetry == COLOUR_UNKNOWN);
    s = hd60s_parse_avi(0, 0x40, 0x80); /* no InfoFrame (DVI): RGB, no colorimetry */
    assert(s.format == INPUT_RGB && s.colorimetry == COLOUR_UNKNOWN);
}

static void csc_plan_is_data_driven(void)
{
    struct hd60s_csc hd, sd, yuv, yuv_sd;
    hd60s_csc_plan(&(struct hd60s_colour_cfg){INPUT_RGB, 0}, &hd);
    hd60s_csc_plan(&(struct hd60s_colour_cfg){INPUT_RGB, 1}, &sd);
    hd60s_csc_plan(&(struct hd60s_colour_cfg){INPUT_YCBCR444, 0}, &yuv);
    hd60s_csc_plan(&(struct hd60s_colour_cfg){INPUT_YCBCR422, 1}, &yuv_sd);
    assert(hd.mode == 0x40 && sd.mode == 0x40 && yuv.mode == 0x62);
    assert(memcmp(&hd, &sd, sizeof hd) != 0);
    assert(yuv.m[0] == 0x1000 && yuv.m[4] == 0x1000 && yuv.m[8] == 0x1000 && yuv.m[1] == 0);
    assert(memcmp(&yuv, &yuv_sd, sizeof yuv) == 0); /* pass-through ignores size and subsampling */
}

static void frame_colour_follows_source_for_ycbcr(void)
{
    struct hd60s_colour_cfg rgb_hd = {INPUT_RGB, 0}, rgb_sd = {INPUT_RGB, 1};
    struct hd60s_colour_cfg y_hd = {INPUT_YCBCR444, 0}, y_sd = {INPUT_YCBCR422, 1};
    assert(hd60s_frame_colour(&rgb_hd, COLOUR_BT601) == COLOUR_BT709); /* device encodes RGB itself */
    assert(hd60s_frame_colour(&rgb_sd, COLOUR_BT709) == COLOUR_BT601);
    assert(hd60s_frame_colour(&y_hd, COLOUR_BT601) == COLOUR_BT601);
    assert(hd60s_frame_colour(&y_sd, COLOUR_BT709) == COLOUR_BT709);
    assert(hd60s_frame_colour(&y_hd, COLOUR_UNKNOWN) == COLOUR_BT709);
    assert(hd60s_frame_colour(&y_sd, COLOUR_UNKNOWN) == COLOUR_BT601);
}

static void policy_follows_source_colour_format(void)
{
    struct hd60s d;
    struct hd60s_parser p = {0};
    struct hd60s_policy pol;
    boot(&d, 0);
    assert(hd60s_init(&d) == 0 && hd60s_start(&d) == 0);
    p.last_w = 1920, p.last_h = 1080;
    hd60s_policy_begin(&pol, 0);
    hd60s_policy_tick(&d, &p, &pol, 0);
    assert(p.input_format == INPUT_RGB && p.colour == COLOUR_BT709 && fk.mst[0][0x92] == 0x40);

    fk.mst[2][0x0b] = 0x08;
    fk.mst[2][0x48] = 0x40;
    fk.mst[2][0x49] = 0x40; /* 601 flagged on an HD frame */
    hd60s_policy_tick(&d, &p, &pol, 1000);
    assert(d.colour.input == INPUT_YCBCR444 && fk.mst[0][0x92] == 0x62);
    assert(p.input_format == INPUT_YCBCR444 && p.colour == COLOUR_BT601);
    int before = fk.requests;
    hd60s_policy_tick(&d, &p, &pol, 1100);
    assert(fk.requests == before);

    fk.mst[2][0x48] = 0x20;
    fk.mst[2][0x49] = 0x00; /* 4:2:2, unspecified: HD falls back to 709 */
    hd60s_policy_tick(&d, &p, &pol, 2000);
    assert(p.input_format == INPUT_YCBCR422 && p.colour == COLOUR_BT709 && fk.mst[0][0x92] == 0x62);

    fk.mst[2][0x48] = 0x00; /* back to RGB mid-stream */
    hd60s_policy_tick(&d, &p, &pol, 3000);
    assert(p.input_format == INPUT_RGB && p.colour == COLOUR_BT709 && fk.mst[0][0x92] == 0x40);

    fk.mst[2][0x0b] = 0x00; /* InfoFrame lost while YCbCr: RGB again */
    fk.mst[2][0x48] = 0x40;
    hd60s_policy_tick(&d, &p, &pol, 4000);
    assert(p.input_format == INPUT_RGB);
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
    assert(fk.codec[0x03] == 0 && d.audio.source == AUDIO_LINE);
    assert(hd60s_start(&d) == 0);
    assert(!(fk.codec[0x0e] & 0x80) && fk.codec[0x10] & 0x80);
    assert(fk.codec[0x0c] == 0x74 && fk.codec[0x0a] == 0);
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
    int idle_cost = fk.requests - before;
    fk.mst[0][0x51] = 0x80;
    fk.mst[1][0x01] = 0x04;
    hd60s_policy_tick(&d, &p, &pol, 2000);
    assert(fk.codec[0x03] == 0xa0 && fk.mst[2][0x27] == 0);
    before = fk.requests;
    hd60s_policy_tick(&d, &p, &pol, 3000);
    assert(fk.requests - before == idle_cost);
}

static void transfer_plan_defaults_to_bulk_and_picks_narrowest_sufficient_iso_alt(void)
{
    struct hd60s_xfer_plan p;
    enum hd60s_transfer m = HD60S_ISO;
    int alts[HD60S_ISO_ALTS] = {16384, 32768, 39936};
    assert(hd60s_transfer_parse("bulk", &m) && m == HD60S_BULK);
    assert(hd60s_transfer_parse("iso", &m) && m == HD60S_ISO);
    assert(!hd60s_transfer_parse("usb", &m));
    assert(hd60s_xfer_plan(HD60S_BULK, alts, &p) == 0 && p.alt == 4 && p.packets == 0);
    assert(hd60s_xfer_plan(HD60S_ISO, alts, &p) == 0 && p.alt == 2 && p.packets == 160 && p.packet_bytes == 32768 && p.buffers == 4);
    int none[HD60S_ISO_ALTS] = {0, 16384, 0};
    assert(hd60s_xfer_plan(HD60S_ISO, none, &p) < 0);
    struct hd60s d = {0};
    assert(d.usb.transfer == HD60S_BULK);
}

int main(void)
{
    transfer_plan_defaults_to_bulk_and_picks_narrowest_sufficient_iso_alt();
    init_runs_vendor_sequence_and_reads_mcu_version();
    init_ignores_vendor_init_failures();
    first_transport_error_fails_start();
    policy_switches_colour_matrix_once_per_change();
    avi_parse_covers_each_format_and_colorimetry();
    csc_plan_is_data_driven();
    frame_colour_follows_source_for_ycbcr();
    policy_follows_source_colour_format();
    policy_reads_aspect_once_a_second();
    policy_finds_mode_from_a_stable_window();
    start_routes_hdmi_audio_by_default();
    audio_settings_apply_at_start_and_live();
    policy_reruns_routing_when_the_source_becomes_hdmi();
    puts("test_device: ok");
    return 0;
}
