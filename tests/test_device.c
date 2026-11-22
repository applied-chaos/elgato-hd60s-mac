#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/device.h"

struct fake {
    uint8_t mst[256][256];
    uint8_t bank;
    uint8_t pend_addr, pend_sub[8];
    int pend_len, pend_rl;
    int requests, fail_at, slept_ms;
    uint8_t first_req[3];
};

static void store(struct fake *f, uint8_t addr, const uint8_t *w, int n)
{
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
    s = hd60s_parse_avi(1, 0x1f, 0x3f, 0); /* other bits ignored */
    assert(s.format == INPUT_RGB && s.colorimetry == COLOUR_UNKNOWN);
    s = hd60s_parse_avi(0, 0x40, 0x80, 0); /* no InfoFrame (DVI): RGB, no colorimetry */
    assert(s.format == INPUT_RGB && s.colorimetry == COLOUR_UNKNOWN);
}

static void csc_plan_is_data_driven(void)
{
    struct hd60s_csc hd, sd, yuv, yuv_sd;
    hd60s_csc_plan(&(struct hd60s_colour_cfg){INPUT_RGB, 0, RANGE_AUTO, 0}, &hd);
    hd60s_csc_plan(&(struct hd60s_colour_cfg){INPUT_RGB, 1, RANGE_AUTO, 0}, &sd);
    hd60s_csc_plan(&(struct hd60s_colour_cfg){INPUT_YCBCR444, 0, RANGE_AUTO, 0}, &yuv);
    hd60s_csc_plan(&(struct hd60s_colour_cfg){INPUT_YCBCR422, 1, RANGE_AUTO, 0}, &yuv_sd);
    assert(hd.mode == 0x40 && sd.mode == 0x40 && yuv.mode == 0x62);
    assert(memcmp(&hd, &sd, sizeof hd) != 0);
    assert(yuv.m[0] == 0x1000 && yuv.m[4] == 0x1000 && yuv.m[8] == 0x1000 && yuv.m[1] == 0);
    assert(memcmp(&yuv, &yuv_sd, sizeof yuv) == 0); /* pass-through ignores size and subsampling */
}

static void frame_colour_follows_source_for_ycbcr(void)
{
    struct hd60s_colour_cfg rgb_hd = {INPUT_RGB, 0, RANGE_AUTO, 0}, rgb_sd = {INPUT_RGB, 1, RANGE_AUTO, 0};
    struct hd60s_colour_cfg y_hd = {INPUT_YCBCR444, 0, RANGE_AUTO, 0}, y_sd = {INPUT_YCBCR422, 1, RANGE_AUTO, 0};
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

static struct hd60s_csc plan(enum hd60s_input_format in, int sd, enum hd60s_range r, int quant)
{
    struct hd60s_csc c;
    hd60s_csc_plan(&(struct hd60s_colour_cfg){in, sd, r, quant}, &c);
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
        assert(memcmp(&a, q == 2 ? &full : &std, sizeof a) == 0); /* only Q=2 (full range) is compressed */
        struct hd60s_csc s = plan(INPUT_RGB, 0, RANGE_STANDARD, q), e = plan(INPUT_RGB, 0, RANGE_EXPANDED, q);
        assert(memcmp(&s, &std, sizeof s) == 0 && memcmp(&e, &full, sizeof e) == 0); /* overrides ignore Q */
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
    assert(d.settings.range == RANGE_AUTO && fk.mst[0][0x92] == 0x40);

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

    d.settings.range = RANGE_EXPANDED; /* live change from the app */
    hd60s_policy_tick(&d, &p, &pol, 3000);
    assert((uint16_t)(fk.mst[0][0x93] << 8 | fk.mst[0][0x94]) == full0);
}

static void settings_round_trip_and_reject_bad_input(void)
{
    char buf[64];
    struct hd60s_settings s;
    for (enum hd60s_range r = RANGE_AUTO; r <= RANGE_EXPANDED; r++) {
        hd60s_settings_print(&(struct hd60s_settings){r}, buf, sizeof buf);
        assert(hd60s_settings_parse(buf, &s) == 0 && s.range == r);
    }
    assert(hd60s_settings_parse("{ \"range\" : \"expanded\" }", &s) == 0 && s.range == RANGE_EXPANDED);
    s.range = RANGE_STANDARD;
    assert(hd60s_settings_parse("{\"range\":\"wide\"}", &s) < 0 && s.range == RANGE_STANDARD);
    assert(hd60s_settings_parse("{}", &s) < 0 && hd60s_settings_parse("", &s) < 0);
    assert(hd60s_settings_parse(NULL, &s) < 0);
    assert(hd60s_range_from_name("auto") == RANGE_AUTO && hd60s_range_from_name("standard") == RANGE_STANDARD);
    assert(hd60s_range_from_name("full") < 0);
}

int main(void)
{
    init_runs_vendor_sequence_and_reads_mcu_version();
    init_ignores_vendor_init_failures();
    first_transport_error_fails_start();
    policy_switches_colour_matrix_once_per_change();
    avi_parse_covers_each_format_and_colorimetry();
    csc_plan_is_data_driven();
    frame_colour_follows_source_for_ycbcr();
    policy_follows_source_colour_format();
    colour_range_scales_the_matrix();
    auto_range_follows_source_quantisation();
    range_scaling_reaches_hardware_and_follows_the_source();
    settings_round_trip_and_reject_bad_input();
    policy_reads_aspect_once_a_second();
    policy_finds_mode_from_a_stable_window();
    puts("test_device: ok");
    return 0;
}
