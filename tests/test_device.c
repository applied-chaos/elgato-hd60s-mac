#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "../src/device.h"

struct fake {
    uint8_t mst[256][256];
    uint8_t codec[256];
    uint8_t bank;
    uint8_t pend_addr, pend_sub[8];
    int pend_len, pend_rl;
    int requests, fail_at, failing, avi_latch, slept_ms;
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
    if (++f->requests == f->fail_at || f->failing)
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
        else if (f->pend_addr == 0x9c) {
            buf[0] = f->mst[f->bank][f->pend_sub[0]];
            if (f->avi_latch && f->bank == 2 && f->pend_sub[0] == 0x0b)
                f->mst[2][0x0b] &= (uint8_t)~0x08;
        }
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
    struct hd60s_source s = hd60s_parse_avi(1, 0x00, 0x00, 0);
    assert(s.format == INPUT_RGB && s.colorimetry == COLOUR_UNKNOWN);
    s = hd60s_parse_avi(1, 0x20, 0x40, 0);
    assert(s.format == INPUT_YCBCR422 && s.colorimetry == COLOUR_BT601);
    s = hd60s_parse_avi(1, 0x40, 0x80, 0);
    assert(s.format == INPUT_YCBCR444 && s.colorimetry == COLOUR_BT709);
    s = hd60s_parse_avi(1, 0x40, 0xc0, 0);
    assert(s.format == INPUT_YCBCR444 && s.colorimetry == COLOUR_UNKNOWN);
    s = hd60s_parse_avi(1, 0x60, 0x80, 0); /* reserved Y1Y0: Elgato treats it as RGB */
    assert(s.format == INPUT_RGB);
    s = hd60s_parse_avi(1, 0x1f, 0x3f, 0);
    assert(s.format == INPUT_RGB && s.colorimetry == COLOUR_UNKNOWN);
    s = hd60s_parse_avi(0, 0x40, 0x80, 0); /* no InfoFrame (DVI): RGB, no colorimetry */
    assert(s.format == INPUT_RGB && s.colorimetry == COLOUR_UNKNOWN);
}

static void csc_plan_is_data_driven(void)
{
    struct hd60s_csc hd, sd, yuv, yuv_sd;
    hd60s_csc_plan(&(struct hd60s_colour_cfg){INPUT_RGB, 0, RANGE_AUTO, 0, {0}}, &hd);
    hd60s_csc_plan(&(struct hd60s_colour_cfg){INPUT_RGB, 1, RANGE_AUTO, 0, {0}}, &sd);
    hd60s_csc_plan(&(struct hd60s_colour_cfg){INPUT_YCBCR444, 0, RANGE_AUTO, 0, {0}}, &yuv);
    hd60s_csc_plan(&(struct hd60s_colour_cfg){INPUT_YCBCR422, 1, RANGE_AUTO, 0, {0}}, &yuv_sd);
    assert(hd.mode == 0x40 && sd.mode == 0x40 && yuv.mode == 0x62);
    assert(memcmp(&hd, &sd, sizeof hd) != 0);
    assert(yuv.m[0] == 0x1000 && yuv.m[4] == 0x1000 && yuv.m[8] == 0x1000 && yuv.m[1] == 0);
    assert(memcmp(&yuv, &yuv_sd, sizeof yuv) == 0); /* pass-through ignores size and subsampling */
}

static void frame_colour_follows_source_for_ycbcr(void)
{
    struct hd60s_colour_cfg rgb_hd = {INPUT_RGB, 0, RANGE_AUTO, 0, {0}}, rgb_sd = {INPUT_RGB, 1, RANGE_AUTO, 0, {0}};
    struct hd60s_colour_cfg y_hd = {INPUT_YCBCR444, 0, RANGE_AUTO, 0, {0}}, y_sd = {INPUT_YCBCR422, 1, RANGE_AUTO, 0, {0}};
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

static void policy_takes_mode_from_avi_vic_when_encrypted_and_no_frames(void)
{
    struct hd60s d;
    struct hd60s_parser p = {0};
    struct hd60s_policy pol;
    boot(&d, 0);
    assert(hd60s_init(&d) == 0 && hd60s_start(&d) == 0);
    hd60s_policy_begin(&pol, 0);
    fk.mst[0][0x51] = 0x80;
    fk.mst[1][0x34] = 0x80;
    fk.mst[2][0x0b] = 0x08;
    fk.mst[2][0x4b] = 16;
    hd60s_policy_tick(&d, &p, &pol, 1000);
    hd60s_policy_tick(&d, &p, &pol, 2000);
    assert(pol.mode && pol.mode->vic == 16);
    fk.mst[2][0x4b] = 4;
    hd60s_policy_tick(&d, &p, &pol, 4000);
    assert(pol.mode && pol.mode->vic == 4);
    fk.mst[2][0x0b] = 0;
    hd60s_policy_tick(&d, &p, &pol, 6000);
    assert(!pol.mode);
}

static void encrypted_mode_survives_the_avi_latch_clearing_on_read(void)
{
    struct hd60s d;
    struct hd60s_parser p = {0};
    struct hd60s_policy pol;
    boot(&d, 0);
    assert(hd60s_init(&d) == 0 && hd60s_start(&d) == 0);
    hd60s_policy_begin(&pol, 0);
    fk.avi_latch = 1;
    fk.mst[0][0x51] = 0x80;
    fk.mst[1][0x01] = 0x04;
    fk.mst[1][0x34] = 0x80;
    fk.mst[2][0x0b] = 0x08;
    fk.mst[2][0x4b] = 16;
    hd60s_policy_tick(&d, &p, &pol, 1000);
    hd60s_policy_tick(&d, &p, &pol, 2000);
    assert(pol.mode && pol.mode->vic == 16);
    hd60s_policy_tick(&d, &p, &pol, 4000);
    assert(pol.mode && pol.mode->vic == 16);
}

static void clear_source_ignores_avi_vic_without_frames(void)
{
    struct hd60s d;
    struct hd60s_parser p = {0};
    struct hd60s_policy pol;
    boot(&d, 0);
    assert(hd60s_init(&d) == 0 && hd60s_start(&d) == 0);
    hd60s_policy_begin(&pol, 0);
    fk.mst[0][0x51] = 0x80;
    fk.mst[2][0x0b] = 0x08;
    fk.mst[2][0x4b] = 16;
    hd60s_policy_tick(&d, &p, &pol, 1000);
    hd60s_policy_tick(&d, &p, &pol, 2000);
    assert(!pol.mode);
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

static struct hd60s_csc plan(enum hd60s_input_format in, int sd, enum hd60s_range r, int quant)
{
    struct hd60s_csc c;
    hd60s_csc_plan(&(struct hd60s_colour_cfg){in, sd, r, quant, {0}}, &c);
    return c;
}

static void colour_range_scales_the_matrix(void)
{
    struct hd60s_csc std = plan(INPUT_YCBCR444, 0, RANGE_STANDARD, 0);
    struct hd60s_csc exp = plan(INPUT_YCBCR444, 0, RANGE_EXPANDED, 0);
    assert(std.m[0] == 0x1000 && std.mode == 0x62 && std.m[10] == plan(INPUT_YCBCR444, 0, RANGE_AUTO, 0).m[10]);
    assert(exp.m[0] == 0x0dc0 && exp.m[4] == 0x0dc0 && exp.m[8] == 0x0dc0 && exp.m[1] == 0); /* full to limited: 219/255 */
    assert(exp.mode == 0x62 && exp.m[10] == (uint16_t)(((0x40 + 0x400) & 0x7fc0) | 1));

    struct hd60s_csc rgb = plan(INPUT_RGB, 0, RANGE_STANDARD, 0), rgb_exp = plan(INPUT_RGB, 0, RANGE_EXPANDED, 0);
    assert(rgb.m[0] > 0 && rgb.m[0] < 0x3fff && rgb_exp.m[0] == (uint16_t)(rgb.m[0] * 0xdc0 >> 12));
    assert(rgb_exp.mode == 0x40 && memcmp(&rgb, &rgb_exp, sizeof rgb) != 0);
}

static void auto_range_follows_source_quantisation(void)
{
    struct hd60s_csc std = plan(INPUT_RGB, 0, RANGE_STANDARD, 0), full = plan(INPUT_RGB, 0, RANGE_EXPANDED, 0);
    for (int q = 0; q < 4; q++) {
        struct hd60s_csc a = plan(INPUT_RGB, 0, RANGE_AUTO, q);
        assert(memcmp(&a, q == 2 ? &full : &std, sizeof a) == 0);
        struct hd60s_csc s = plan(INPUT_RGB, 0, RANGE_STANDARD, q), e = plan(INPUT_RGB, 0, RANGE_EXPANDED, q);
        assert(memcmp(&s, &std, sizeof s) == 0 && memcmp(&e, &full, sizeof e) == 0);
    }
}

static void range_scaling_reaches_hardware_and_follows_the_source(void)
{
    struct hd60s d;
    struct hd60s_parser p = {0};
    struct hd60s_policy pol;
    boot(&d, 0);
    assert(hd60s_init(&d) == 0 && hd60s_start(&d) == 0);
    p.last_w = 1920, p.last_h = 1080;
    hd60s_policy_begin(&pol, 0);
    hd60s_policy_tick(&d, &p, &pol, 0);
    uint16_t std0 = (uint16_t)(fk.mst[0][0x93] << 8 | fk.mst[0][0x94]);
    assert(d.published_settings.range == RANGE_AUTO && fk.mst[0][0x92] == 0x40);

    fk.mst[2][0x0b] = 0x08;
    fk.mst[2][0x4a] = 0x08; /* Q = 2, full range */
    hd60s_policy_tick(&d, &p, &pol, 1000);
    uint16_t full0 = (uint16_t)(fk.mst[0][0x93] << 8 | fk.mst[0][0x94]);
    assert(full0 == (uint16_t)(std0 * 0xdc0 >> 12) && d.colour.quant == 2);
    int before = fk.requests;
    hd60s_policy_tick(&d, &p, &pol, 1100);
    assert(fk.requests == before);

    fk.mst[2][0x4a] = 0x04; /* Q = 1, limited */
    hd60s_policy_tick(&d, &p, &pol, 2000);
    assert((uint16_t)(fk.mst[0][0x93] << 8 | fk.mst[0][0x94]) == std0);

    d.published_settings.range = RANGE_EXPANDED;
    hd60s_policy_tick(&d, &p, &pol, 3000);
    assert((uint16_t)(fk.mst[0][0x93] << 8 | fk.mst[0][0x94]) == full0);
}

static void picture_controls_follow_fun_103ea(void)
{
    /* Expected words come from a literal port of FUN_103ea's decompile applied to the BT.709 matrix; deltas are from Elgato's default bytes. */
    static const struct {
        struct hd60s_picture p;
        uint16_t m[12];
    } cases[] = {
        {{0, 0, 0, 0}, {0x802, 0x365, 0x7e28, 0x78b9, 0xb65, 0x79d6, 0x7f45, 0x127, 0x802, 0x2000, 0x41, 0x2000}},
        {{63, 0, 0, 0}, {0x802, 0x365, 0x7e28, 0x78b9, 0xb65, 0x79d6, 0x7f45, 0x127, 0x802, 0x2000, 0x1401, 0x2000}},  /* bri 0xc0 */
        {{-65, 0, 0, 0}, {0x802, 0x365, 0x7e28, 0x78b9, 0xb65, 0x79d6, 0x7f45, 0x127, 0x802, 0x2000, 0x6c01, 0x2000}},  /* bri 0x40 */
        {{0, 64, 0, 0}, {0x802, 0x517, 0x7e28, 0x78b9, 0x1117, 0x79d6, 0x7f45, 0x1ba, 0x802, 0x2000, 0x41, 0x2000}},  /* con 0xc0 */
        {{0, 0, -128, 0}, {0xf, 0x365, 0x7ffc, 0x7ff1, 0xb65, 0x7ff4, 0x7fff, 0x127, 0xf, 0x2000, 0x41, 0x2000}},  /* sat 0 */
        {{0, 0, 64, 0}, {0xc03, 0x365, 0x7d3c, 0x7515, 0xb65, 0x76c1, 0x7ee7, 0x127, 0xc03, 0x2000, 0x41, 0x2000}},  /* sat 0xc0 */
        {{0, 0, 0, 16}, {0x7ac, 0x365, 0x7d0f, 0x77f0, 0xb65, 0x7ae9, 0x63, 0x127, 0x807, 0x2000, 0x41, 0x2000}},  /* hue 0x90 */
        {{0, 0, 0, -16}, {0x82f, 0x365, 0x7f4a, 0x79a7, 0xb65, 0x78e2, 0x7e2a, 0x127, 0x7d3, 0x2000, 0x41, 0x2000}},  /* hue 0x70 */
        {{0, 0, 0, -128}, {0x570, 0x365, 0x627, 0x1f1, 0xb65, 0x76aa, 0x789e, 0x127, 0x32d, 0x2000, 0x41, 0x2000}},  /* hue 0 */
        {{0, 0, 0, 127}, {0x233, 0x365, 0x7816, 0x771d, 0xb65, 0x373, 0x6af, 0x127, 0x476, 0x2000, 0x41, 0x2000}},  /* hue 0xff */
        {{127, 127, 127, 127}, {0x462, 0x6c3, 0x703c, 0x6e4b, 0x16b2, 0x6e0, 0xd51, 0x24b, 0x8e3, 0x2000, 0x2781, 0x2000}},  /* all 0xff */
        {{-128, -128, -128, -128}, {0xa, 0x0, 0xb, 0x3, 0x0, 0x7fed, 0x7ff2, 0x0, 0x6, 0x2000, 0x5881, 0x2000}},  /* all minimum (brightness byte 1) */
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        struct hd60s_csc c;
        hd60s_csc_plan(&(struct hd60s_colour_cfg){INPUT_RGB, 0, RANGE_STANDARD, 0, cases[i].p}, &c);
        assert(c.mode == 0x40 && memcmp(c.m, cases[i].m, sizeof c.m) == 0);
    }
}

enum { COMPRESS_OFFSET_WORD = 0x400, DEFAULT_BRIGHTNESS_WORD = 0x41, BRIGHT_63_WORD = 0x1401 };

static void picture_controls_reach_hardware_live_and_survive_a_range_change(void)
{
    struct hd60s d;
    struct hd60s_parser p = {0};
    struct hd60s_policy pol;
    boot(&d, 0);
    assert(hd60s_init(&d) == 0 && hd60s_start(&d) == 0);
    p.last_w = 1920, p.last_h = 1080;
    hd60s_policy_begin(&pol, 0);
    hd60s_policy_tick(&d, &p, &pol, 0);
    assert((fk.mst[0][0xa7] << 8 | fk.mst[0][0xa8]) == DEFAULT_BRIGHTNESS_WORD);

    d.published_settings.picture.brightness = 63;
    hd60s_policy_tick(&d, &p, &pol, 1000);
    assert((fk.mst[0][0xa7] << 8 | fk.mst[0][0xa8]) == BRIGHT_63_WORD);
    int before = fk.requests;
    hd60s_policy_tick(&d, &p, &pol, 1100);
    assert(fk.requests == before);

    d.published_settings.range = RANGE_EXPANDED;
    d.published_settings.picture.brightness = 0;
    fk.mst[2][0x0b] = 0x08;
    fk.mst[2][0x4a] = 0x08;
    hd60s_policy_tick(&d, &p, &pol, 2000);
    assert((fk.mst[0][0xa7] << 8 | fk.mst[0][0xa8]) == COMPRESS_OFFSET_WORD + DEFAULT_BRIGHTNESS_WORD);
    d.published_settings.picture.brightness = 63;
    hd60s_policy_tick(&d, &p, &pol, 3000);
    assert((fk.mst[0][0xa7] << 8 | fk.mst[0][0xa8]) == COMPRESS_OFFSET_WORD + BRIGHT_63_WORD);
}

static void settings_round_trip_and_reject_bad_input(void)
{
    char buf[128];
    struct hd60s_settings s;
    for (enum hd60s_range r = RANGE_AUTO; r <= RANGE_EXPANDED; r++) {
        hd60s_settings_print(&(struct hd60s_settings){r, {0}, {0}}, buf, sizeof buf);
        assert(hd60s_settings_parse(buf, &s) == 0 && s.range == r);
    }
    assert(hd60s_settings_parse("{ \"range\" : \"expanded\" }", &s) == 0 && s.range == RANGE_EXPANDED);
    s.range = RANGE_STANDARD;
    assert(hd60s_settings_parse("{\"range\":\"wide\"}", &s) < 0 && s.range == RANGE_STANDARD);
    assert(hd60s_settings_parse("{}", &s) < 0 && hd60s_settings_parse("", &s) < 0);
    assert(hd60s_settings_parse(NULL, &s) < 0);
    assert(hd60s_range_from_name("auto") == RANGE_AUTO && hd60s_range_from_name("standard") == RANGE_STANDARD);
    assert(hd60s_range_from_name("full") < 0);

    struct hd60s_settings pic = {RANGE_STANDARD, {-128, 5, 127, -1}, {0}};
    hd60s_settings_print(&pic, buf, sizeof buf);
    assert(hd60s_settings_parse(buf, &s) == 0 && s.range == RANGE_STANDARD && memcmp(&s.picture, &pic.picture, sizeof pic.picture) == 0);
    assert(hd60s_settings_parse("{\"range\":\"auto\"}", &s) == 0 && !s.picture.hue && !s.picture.brightness);
    s.picture.hue = 9;
    assert(hd60s_settings_parse("{\"range\":\"auto\",\"hue\":128}", &s) < 0 && s.picture.hue == 9);
    assert(hd60s_settings_parse("{\"range\":\"auto\",\"contrast\":\"x\"}", &s) < 0);
}

static void audio_settings_round_trip_and_reject_bad_input(void)
{
    char buf[192];
    struct hd60s_settings s = {0};
    assert(hd60s_settings_parse("{\"range\":\"auto\"}", &s) == 0);
    assert(s.audio.source == AUDIO_HDMI && s.audio.gain_offset == 0 && !s.audio.mute);
    struct hd60s_settings in = {RANGE_EXPANDED, {1, 2, 3, 4}, {AUDIO_MIX, 10, 1}};
    hd60s_settings_print(&in, buf, sizeof buf);
    assert(strstr(buf, "\"source\":\"mix\"") && strstr(buf, "\"gain\":-2") && strstr(buf, "\"mute\":true"));
    assert(hd60s_settings_parse(buf, &s) == 0 && memcmp(&s, &in, sizeof s) == 0);
    hd60s_settings_print(&(struct hd60s_settings){0}, buf, sizeof buf);
    assert(strstr(buf, "\"gain\":-12") && strstr(buf, "\"mute\":false"));
    assert(hd60s_settings_parse("{\"range\":\"auto\",\"source\":\"line\",\"gain\":24,\"mute\":false}", &s) == 0);
    assert(s.audio.source == AUDIO_LINE && s.audio.gain_offset == 36 && !s.audio.mute);
    struct hd60s_settings keep = s;
    assert(hd60s_settings_parse("{\"range\":\"auto\",\"source\":\"both\"}", &s) < 0);
    assert(hd60s_settings_parse("{\"range\":\"auto\",\"gain\":25}", &s) < 0);
    assert(hd60s_settings_parse("{\"range\":\"auto\",\"gain\":-25}", &s) < 0);
    assert(hd60s_settings_parse("{\"range\":\"auto\",\"gain\":\"x\"}", &s) < 0);
    assert(hd60s_settings_parse("{\"range\":\"auto\",\"mute\":1}", &s) < 0);
    assert(memcmp(&s, &keep, sizeof s) == 0);
}

static void policy_applies_audio_settings_live(void)
{
    struct hd60s d;
    struct hd60s_parser p = {0};
    struct hd60s_policy pol;
    boot(&d, 0);
    assert(hd60s_init(&d) == 0 && hd60s_start(&d) == 0);
    hd60s_policy_begin(&pol, 0);
    hd60s_policy_tick(&d, &p, &pol, 0);
    assert(fk.codec[0x03] == 0xa1 && d.audio.source == AUDIO_HDMI);
    d.published_settings.audio = (struct audio_pref){AUDIO_LINE, 12, 0};
    hd60s_policy_tick(&d, &p, &pol, 10);
    assert(d.audio.source == AUDIO_LINE && d.audio.gain_half_db == 0);
    assert(!(fk.codec[0x0e] & 0x80) && fk.codec[0x10] & 0x80 && fk.codec[0x0c] == 0);
    d.published_settings.audio.mute = 1;
    hd60s_policy_tick(&d, &p, &pol, 20);
    assert(fk.codec[0x0e] & 0x80 && fk.codec[0x0f] & 0x80);
    int before = fk.requests;
    hd60s_policy_tick(&d, &p, &pol, 30);
    assert(fk.requests == before);
}

static void link_flags_decode_hdcp_and_dvi(void)
{
    /* b1:0x01 bit2 is set for an HDMI source (AVI present) and clear for DVI; measured on the Pi with and without an HDMI EDID. */
    struct hd60s_link l = hd60s_link_from_regs(1, 0x04, 0x00);
    assert(!l.hdcp && !l.dvi);
    l = hd60s_link_from_regs(1, 0x00, 0x00);
    assert(!l.hdcp && l.dvi);
    l = hd60s_link_from_regs(1, 0x05, 0x00);
    assert(l.hdcp && !l.dvi);
    l = hd60s_link_from_regs(1, 0x04, 0x80);
    assert(l.hdcp && !l.dvi);
    l = hd60s_link_from_regs(1, 0x04, 0x7f);
    assert(!l.hdcp);
    l = hd60s_link_from_regs(1, 0xfa, 0x00);
    assert(!l.hdcp && l.dvi);
    l = hd60s_link_from_regs(0, 0x00, 0x00);
    assert(!l.dvi);
    assert(hd60s_link_from_regs(1, 0x04, 0).hdmi && !hd60s_link_from_regs(1, 0x04, 0).dvi);
    assert(!hd60s_link_from_regs(1, 0x00, 0).hdmi && hd60s_link_from_regs(1, 0x00, 0).dvi);
    assert(!hd60s_link_from_regs(0, 0x04, 0).hdmi && !hd60s_link_from_regs(0, 0x04, 0).dvi);
}

static void policy_tracks_the_source_link(void)
{
    struct hd60s d;
    struct hd60s_parser p = {0};
    struct hd60s_policy pol;
    boot(&d, 0);
    assert(hd60s_init(&d) == 0 && hd60s_start(&d) == 0);
    hd60s_policy_begin(&pol, 0);
    fk.mst[0][0x51] = 0x80;
    fk.mst[1][0x01] = 0x04;
    hd60s_policy_tick(&d, &p, &pol, 1000);
    assert(!d.link.hdcp && !d.link.dvi);
    fk.mst[1][0x34] = 0x80;
    hd60s_policy_tick(&d, &p, &pol, 2000);
    assert(d.link.hdcp && !d.link.dvi);
    fk.mst[1][0x34] = 0;
    fk.mst[1][0x01] = 0x00;
    hd60s_policy_tick(&d, &p, &pol, 3000);
    assert(!d.link.hdcp && d.link.dvi);
}

static void policy_keeps_the_last_source_when_reads_fail(void)
{
    struct hd60s d;
    struct hd60s_parser p = {0};
    struct hd60s_policy pol;
    boot(&d, 0);
    assert(hd60s_init(&d) == 0 && hd60s_start(&d) == 0);
    p.last_w = 1920, p.last_h = 1080;
    hd60s_policy_begin(&pol, 0);
    fk.mst[0][0x51] = 0x80;
    fk.mst[1][0x01] = 0x04;
    fk.mst[2][0x0b] = 0x08;
    hd60s_policy_tick(&d, &p, &pol, 1000);
    assert(d.colour.input == INPUT_RGB && !d.link.hdcp && d.link.hdmi);
    struct hd60s_colour_cfg colour = d.colour;
    fk.failing = 1;
    hd60s_policy_tick(&d, &p, &pol, 2000);
    fk.failing = 0;
    assert(!memcmp(&colour, &d.colour, sizeof colour) && p.input_format == INPUT_RGB);
    assert(!d.link.hdcp && d.link.hdmi);
}

static void policy_keeps_the_source_through_an_hdmi_infoframe_dropout(void)
{
    struct hd60s d;
    struct hd60s_parser p = {0};
    struct hd60s_policy pol;
    boot(&d, 0);
    assert(hd60s_init(&d) == 0 && hd60s_start(&d) == 0);
    p.last_w = 1920, p.last_h = 1080;
    hd60s_policy_begin(&pol, 0);
    fk.mst[0][0x51] = 0x80;
    fk.mst[1][0x01] = 0x04;
    fk.mst[2][0x0b] = 0x08;
    fk.mst[2][0x48] = 0x80;
    fk.mst[2][0x49] = 0x28;
    fk.mst[2][0x4a] = 0x08;
    hd60s_policy_tick(&d, &p, &pol, 1000);
    assert(d.colour.quant == 2 && p.wide);
    fk.mst[2][0x0b] = 0;
    hd60s_policy_tick(&d, &p, &pol, 2000);
    hd60s_policy_tick(&d, &p, &pol, 2100);
    assert(d.colour.quant == 2 && p.wide);
    fk.mst[1][0x01] = 0x00;
    hd60s_policy_tick(&d, &p, &pol, 3000);
    assert(d.link.dvi && d.colour.quant == 0 && !p.wide);
}

static void settings_wire_round_trips_and_validates(void)
{
    assert(sizeof(struct hd60s_settings_wire) == 12);
    struct hd60s_settings in = {RANGE_EXPANDED, {-128, 5, 127, -1}, {AUDIO_MIX, -7, 1}}, out = {0};
    struct hd60s_settings_wire w;
    hd60s_settings_to_wire(&in, &w);
    assert(w.version == HD60S_SETTINGS_WIRE_VERSION && !w.reserved[0] && !w.reserved[1]);
    assert(hd60s_settings_from_wire(&w, sizeof w, &out) == 0 && hd60s_settings_equal(&in, &out));
    struct hd60s_settings zero = {0};
    hd60s_settings_to_wire(&zero, &w);
    assert(hd60s_settings_from_wire(&w, sizeof w, &out) == 0 && hd60s_settings_equal(&zero, &out));
    assert(!hd60s_settings_equal(&in, &zero));

    struct hd60s_settings keep = in, got = in;
    hd60s_settings_to_wire(&in, &w);
    struct hd60s_settings_wire bad = w;
    assert(hd60s_settings_from_wire(&w, sizeof w - 1, &got) < 0 && hd60s_settings_from_wire(&w, sizeof w + 1, &got) < 0);
    bad = w, bad.version = HD60S_SETTINGS_WIRE_VERSION + 1;
    assert(hd60s_settings_from_wire(&bad, sizeof bad, &got) < 0);
    bad = w, bad.range = RANGE_COUNT;
    assert(hd60s_settings_from_wire(&bad, sizeof bad, &got) < 0);
    bad = w, bad.audio_source = 2;
    assert(hd60s_settings_from_wire(&bad, sizeof bad, &got) < 0);
    bad = w, bad.mute = 2;
    assert(hd60s_settings_from_wire(&bad, sizeof bad, &got) < 0);
    bad = w, bad.reserved[1] = 1;
    assert(hd60s_settings_from_wire(&bad, sizeof bad, &got) < 0);
    assert(hd60s_settings_from_wire(NULL, sizeof w, &got) < 0 && hd60s_settings_equal(&got, &keep));
}

static const struct hd60s_settings pattern[2] = {
    {RANGE_AUTO, {1, 1, 1, 1}, {AUDIO_HDMI, 1, 0}},
    {RANGE_EXPANDED, {-2, -2, -2, -2}, {AUDIO_LINE, -2, 1}},
};

static void *flip_settings(void *arg)
{
    struct hd60s *d = arg;
    for (int i = 0; i < 400000; i++)
        hd60s_settings_publish(d, &pattern[i & 1]);
    return NULL;
}

static void settings_snapshot_is_never_torn(void)
{
    struct hd60s d;
    boot(&d, 0);
    hd60s_settings_publish(&d, &pattern[0]);
    pthread_t t;
    pthread_create(&t, NULL, flip_settings, &d);
    for (int i = 0; i < 400000; i++) {
        struct hd60s_settings s = hd60s_settings_snapshot(&d);
        assert(hd60s_settings_equal(&s, &pattern[0]) || hd60s_settings_equal(&s, &pattern[1]));
    }
    pthread_join(t, NULL);
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
    colour_range_scales_the_matrix();
    picture_controls_follow_fun_103ea();
    picture_controls_reach_hardware_live_and_survive_a_range_change();
    auto_range_follows_source_quantisation();
    range_scaling_reaches_hardware_and_follows_the_source();
    settings_round_trip_and_reject_bad_input();
    audio_settings_round_trip_and_reject_bad_input();
    policy_applies_audio_settings_live();
    policy_reads_aspect_once_a_second();
    policy_finds_mode_from_a_stable_window();
    policy_takes_mode_from_avi_vic_when_encrypted_and_no_frames();
    encrypted_mode_survives_the_avi_latch_clearing_on_read();
    clear_source_ignores_avi_vic_without_frames();
    start_routes_hdmi_audio_by_default();
    audio_settings_apply_at_start_and_live();
    policy_reruns_routing_when_the_source_becomes_hdmi();
    link_flags_decode_hdcp_and_dvi();
    policy_tracks_the_source_link();
    policy_keeps_the_last_source_when_reads_fail();
    policy_keeps_the_source_through_an_hdmi_infoframe_dropout();
    settings_wire_round_trips_and_validates();
    settings_snapshot_is_never_torn();
    puts("test_device: ok");
    return 0;
}
