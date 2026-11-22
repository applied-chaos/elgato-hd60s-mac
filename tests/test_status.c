#include "../src/status.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static struct hd60s_status healthy(void)
{
    struct hd60s_status s = {0};
    s.present = 1;
    s.speed = SPEED_SUPER;
    s.access = ACCESS_OK;
    s.owner = OWNER_CLI;
    s.fw_valid = 1;
    s.mcu_version = HD60S_KNOWN_MCU[0];
    return s;
}

static int has(const struct hd60s_status *s, enum hd60s_warning w)
{
    return (hd60s_warnings(s) >> w) & 1;
}

static char *render(void (*fn)(const struct hd60s_status *, FILE *), const struct hd60s_status *s)
{
    static char buf[4096];
    FILE *f = fmemopen(buf, sizeof buf, "w");
    memset(buf, 0, sizeof buf);
    fn(s, f);
    fclose(f);
    return buf;
}

int main(void)
{
    struct hd60s_status s = healthy();
    assert(hd60s_warnings(&s) == 0);

    /* No device: nothing else is meaningful. */
    s = (struct hd60s_status){0};
    assert(hd60s_warnings(&s) == 1u << WARN_NO_DEVICE);

    /* USB 2 port, cable or hub; every non-SuperSpeed speed warns, SuperSpeed+ does not. */
    for (int v = 0; v <= 6; v++) {
        s = healthy();
        s.speed = hd60s_speed_from_libusb(v);
        assert(has(&s, WARN_NOT_SUPERSPEED) == (v >= 1 && v < 4)); /* unknown speed is not accused */
    }
    s = healthy();
    s.speed = SPEED_HIGH;
    snprintf(s.port_path, sizeof s.port_path, "2-3.1");
    char msg[256];
    hd60s_warning_message(WARN_NOT_SUPERSPEED, &s, msg, sizeof msg);
    assert(strstr(msg, "USB 2") && strstr(msg, "2-3.1") && strstr(msg, "hub"));

    /* Busy: opened by another process; the owner is named when known. */
    s = healthy();
    s.access = hd60s_access_from_error(-6 /* LIBUSB_ERROR_BUSY */);
    s.owner = OWNER_CAMERA;
    assert(has(&s, WARN_BUSY) && !has(&s, WARN_OPEN_FAILED) );
    hd60s_warning_message(WARN_BUSY, &s, msg, sizeof msg);
    assert(strstr(msg, "camera extension"));
    assert(hd60s_access_from_error(-3 /* ACCESS */) == ACCESS_BUSY);
    assert(hd60s_access_from_error(0) == ACCESS_OK);
    assert(hd60s_access_from_error(-1 /* IO */) == ACCESS_FAILED);
    s.access = ACCESS_FAILED;
    assert(has(&s, WARN_OPEN_FAILED) && !has(&s, WARN_BUSY));

    /* Firmware only judged when read. */
    s = healthy();
    s.mcu_version = 0x010203;
    assert(hd60s_warnings(&s) == 0 && hd60s_firmware_untested(&s)); /* a note, not a warning */
    s.fw_valid = 0;
    assert(!hd60s_firmware_untested(&s));
    s = healthy();
    assert(!hd60s_firmware_untested(&s));
    s.mcu_version = 0x010203;
    char *j0 = render(hd60s_print_json, &s);
    assert(strstr(j0, "\"warnings\":[]") && strstr(j0, "\"notes\":[{\"code\":\"untested_firmware\""));
    assert(strstr(render(hd60s_print_text, &s), "Note:     Untested firmware 01.02.03"));

    /* Owner classification from the owning process's executable path. */
    assert(hd60s_owner_from_path("/Library/SystemExtensions/X/net.mintopia.hd60s.camera.systemextension/Contents/MacOS/net.mintopia.hd60s.camera") == OWNER_CAMERA);
    assert(hd60s_owner_from_path("/Library/SystemExtensions/X/net.mintopia.hd60s.audio.driver.dext/net.mintopia.hd60s.audio") == OWNER_DRIVER);
    assert(hd60s_owner_from_path("/usr/local/bin/ffmpeg") == OWNER_OTHER);
    assert(hd60s_owner_from_path("") == OWNER_UNKNOWN);
    int pid = 0;
    assert(hd60s_parse_owner_pid("pid 26442, net.mintopia.hd6", &pid) && pid == 26442);
    assert(!hd60s_parse_owner_pid("garbage", &pid));

    /* Runts: only the five standard frame sizes count as real frames. */
    assert(hd60s_size_standard(1920, 1080) && hd60s_size_standard(1280, 720));
    assert(hd60s_size_standard(720, 576) && hd60s_size_standard(720, 480));
    assert(hd60s_size_standard(1440, 480));
    assert(!hd60s_size_standard(1280, 12) && !hd60s_size_standard(1920, 1045));

    /* Text report: no signal, then a signal. */
    s = healthy();
    s.signal = SIGNAL_NONE;
    char *t = render(hd60s_print_text, &s);
    assert(strstr(t, "Signal:") && strstr(t, "none") && strstr(t, "Warnings: none"));
    s.signal = SIGNAL_ACTIVE;
    s.width = 1920;
    s.height = 1080;
    s.fps = 59.95;
    s.colour = COLOUR_BT709;
    s.input_format = INPUT_YCBCR422;
    s.audio_rate = 48000;
    s.audio_channels = 2;
    s.frames = 120;
    t = render(hd60s_print_text, &s);
    assert(strstr(t, "1920x1080p 59.95 fps") && strstr(t, "BT.709") && strstr(t, "Input:    YCbCr 4:2:2") && strstr(t, "48000 Hz, 2 channels"));

    /* JSON contract. */
    char *j = render(hd60s_print_json, &s);
    assert(strstr(j, "\"signal\":{\"state\":\"active\",\"width\":1920,\"height\":1080,\"interlaced\":false"));
    assert(strstr(j, "\"warnings\":[]") && strstr(j, "\"input\":\"ycbcr422\",\"colour\":\"bt709\""));
    s.interlaced = 1;
    s.field_order = FIELD_TOP_FIRST;
    s.speed = SPEED_HIGH;
    j = render(hd60s_print_json, &s);
    assert(strstr(j, "\"field_order\":\"top_first\"") && strstr(j, "\"code\":\"not_superspeed\""));
    s = (struct hd60s_status){0};
    j = render(hd60s_print_json, &s);
    assert(strstr(j, "\"present\":false") && strstr(j, "\"code\":\"no_device\"") && strstr(j, "\"state\":\"unknown\""));

    puts("status ok");
    struct { unsigned c; int tenths, ntsc; } rates[] = {
        {0, 0, 0},         {2084, 600, 0},  {2088, 600, 1},  {2500, 500, 0},   {4167, 300, 0},  {4180, 300, 1},
        {5208, 240, 0},    {5230, 240, 1},  {5000, 250, 0},  {1042, 1200, 0},  {1045, 1200, 1}, {1250, 1000, 0},
    };
    for (size_t i = 0; i < sizeof rates / sizeof *rates; i++) {
        struct hd60s_rate r = hd60s_rate_from_vsync(rates[i].c);
        assert(r.tenths == rates[i].tenths && r.ntsc == rates[i].ntsc);
    }
    assert(hd60s_rate_hz((struct hd60s_rate){600, 0}) == 60.0);
    assert(fabs(hd60s_rate_hz((struct hd60s_rate){600, 1}) - 59.94006) < 1e-4);
    assert(fabs(hd60s_exact_fps(59.85, (struct hd60s_rate){600, 0}, 0) - 60.0) < 1e-9);
    assert(fabs(hd60s_exact_fps(60.12, (struct hd60s_rate){600, 1}, 0) - 59.94006) < 1e-4);
    assert(fabs(hd60s_exact_fps(30.04, (struct hd60s_rate){600, 1}, 1) - 29.97003) < 1e-4);
    assert(hd60s_exact_fps(50.2, (struct hd60s_rate){600, 0}, 0) == 50.2);
    assert(hd60s_exact_fps(50.2, (struct hd60s_rate){0, 0}, 0) == 50.2);
    assert(fabs(hd60s_rate_hz((struct hd60s_rate){240, 1}) - 23.97602) < 1e-4);

    return 0;
}
