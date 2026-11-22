#include "status.h"

#include <libusb.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#define WARNING_COUNT (WARN_FIRMWARE + 1)

const uint32_t HD60S_KNOWN_MCU[] = {0x110311}; /* observed on the dev unit */
const int HD60S_KNOWN_MCU_COUNT = 1;

enum hd60s_speed hd60s_speed_from_libusb(int v)
{
    switch (v) {
    case LIBUSB_SPEED_LOW:
        return SPEED_LOW;
    case LIBUSB_SPEED_FULL:
        return SPEED_FULL;
    case LIBUSB_SPEED_HIGH:
        return SPEED_HIGH;
    case LIBUSB_SPEED_SUPER:
        return SPEED_SUPER;
    case LIBUSB_SPEED_SUPER_PLUS:
    case LIBUSB_SPEED_SUPER_PLUS_X2:
        return SPEED_SUPER_PLUS;
    default:
        return SPEED_UNKNOWN;
    }
}

/* macOS reports another process's exclusive claim as ACCESS or BUSY. */
enum hd60s_access hd60s_access_from_error(int e)
{
    if (e == 0)
        return ACCESS_OK;
    return e == LIBUSB_ERROR_BUSY || e == LIBUSB_ERROR_ACCESS ? ACCESS_BUSY : ACCESS_FAILED;
}

enum hd60s_owner hd60s_owner_from_path(const char *path)
{
    if (!path || !*path)
        return OWNER_UNKNOWN;
    if (strstr(path, ".camera.systemextension"))
        return OWNER_CAMERA;
    if (strstr(path, ".dext") || strstr(path, "hd60s.audio"))
        return OWNER_DRIVER;
    return OWNER_OTHER;
}

int hd60s_parse_owner_pid(const char *s, int *pid)
{
    return sscanf(s, "pid %d", pid) == 1;
}

int hd60s_size_standard(int w, int h)
{
    static const int sizes[][2] = {{1920, 1080}, {1280, 720}, {720, 576}, {720, 480}, {1440, 480}};
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++)
        if (w == sizes[i][0] && h == sizes[i][1])
            return 1;
    return 0;
}

static int fw_known(uint32_t v)
{
    for (int i = 0; i < HD60S_KNOWN_MCU_COUNT; i++)
        if (HD60S_KNOWN_MCU[i] == v)
            return 1;
    return 0;
}

uint32_t hd60s_warnings(const struct hd60s_status *s)
{
    if (!s->present)
        return 1u << WARN_NO_DEVICE;
    uint32_t w = 0;
    if (s->speed != SPEED_UNKNOWN && s->speed < SPEED_SUPER)
        w |= 1u << WARN_NOT_SUPERSPEED;
    if (s->access == ACCESS_BUSY)
        w |= 1u << WARN_BUSY;
    if (s->access == ACCESS_FAILED)
        w |= 1u << WARN_OPEN_FAILED;
    if (s->fw_valid && !fw_known(s->mcu_version))
        w |= 1u << WARN_FIRMWARE;
    return w;
}

const char *hd60s_warning_code(enum hd60s_warning w)
{
    switch (w) {
    case WARN_NO_DEVICE:
        return "no_device";
    case WARN_NOT_SUPERSPEED:
        return "not_superspeed";
    case WARN_BUSY:
        return "busy";
    case WARN_OPEN_FAILED:
        return "open_failed";
    case WARN_FIRMWARE:
        return "unexpected_firmware";
    }
    return "";
}

static const char *speed_name(enum hd60s_speed v)
{
    switch (v) {
    case SPEED_LOW:
        return "USB 1.0 low speed (1.5 Mb/s)";
    case SPEED_FULL:
        return "USB 1.1 full speed (12 Mb/s)";
    case SPEED_HIGH:
        return "USB 2.0 high speed (480 Mb/s)";
    case SPEED_SUPER:
        return "USB 3.0 SuperSpeed (5 Gb/s)";
    case SPEED_SUPER_PLUS:
        return "USB 3.1+ SuperSpeed+ (10 Gb/s or more)";
    case SPEED_UNKNOWN:
        break;
    }
    return "unknown speed";
}

static const char *speed_code(enum hd60s_speed v)
{
    static const char *const names[] = {"unknown", "low", "full", "high", "super", "super_plus"};
    return names[v];
}

static const char *owner_name(enum hd60s_owner o)
{
    switch (o) {
    case OWNER_NONE:
        return "nobody";
    case OWNER_CLI:
        return "this tool";
    case OWNER_CAMERA:
        return "camera extension";
    case OWNER_DRIVER:
        return "driver extension";
    case OWNER_OTHER:
        return "another process";
    case OWNER_UNKNOWN:
        break;
    }
    return "unknown";
}

static const char *owner_code(enum hd60s_owner o)
{
    static const char *const names[] = {"nobody", "cli", "camera_extension", "driver_extension", "other", "unknown"};
    return names[o];
}

void hd60s_warning_message(enum hd60s_warning w, const struct hd60s_status *s, char *buf, size_t n)
{
    char at[64] = "";
    if (s->port_path[0])
        snprintf(at, sizeof at, " (port %s)", s->port_path);
    switch (w) {
    case WARN_NO_DEVICE:
        snprintf(buf, n, "No HD60 S found. Check the USB cable and that the device is powered.");
        break;
    case WARN_NOT_SUPERSPEED:
        snprintf(buf, n,
                 "Connected at %s%s, not SuperSpeed. Use a USB 3 port, a USB 3 cable, and no USB 2 hub.",
                 speed_name(s->speed), at);
        break;
    case WARN_BUSY:
        snprintf(buf, n, "Device is in use by %s%s%s. Signal details unavailable until it lets go.",
                 owner_name(s->owner), s->owner_name[0] ? ": " : "", s->owner_name);
        break;
    case WARN_OPEN_FAILED:
        snprintf(buf, n, "Could not open the device. Unplug and replug it.");
        break;
    case WARN_FIRMWARE:
        snprintf(buf, n, "Unexpected MCU firmware %02x.%02x.%02x. Capture may misbehave.",
                 s->mcu_version >> 16 & 0xff, s->mcu_version >> 8 & 0xff, s->mcu_version & 0xff);
        break;
    }
}

static const char *colour_name(enum hd60s_colour c)
{
    return c == COLOUR_BT709 ? "BT.709" : c == COLOUR_BT601 ? "BT.601" : "unknown";
}

static void mode_text(const struct hd60s_status *s, char *buf, size_t n)
{
    snprintf(buf, n, "%dx%d%s %.2f fps", s->width, s->height, s->interlaced ? "i" : "p", s->fps);
}

void hd60s_print_text(const struct hd60s_status *s, FILE *out)
{
    char line[256], mode[64];
    fprintf(out, "Device:   %s", s->present ? "present" : "not found");
    if (s->present)
        fprintf(out, ", %s%s%s", speed_name(s->speed), s->port_path[0] ? ", port " : "", s->port_path);
    fputc('\n', out);
    if (s->fw_valid)
        fprintf(out, "Firmware: MCU %02x.%02x.%02x\n", s->mcu_version >> 16 & 0xff,
                s->mcu_version >> 8 & 0xff, s->mcu_version & 0xff);
    else
        fprintf(out, "Firmware: unknown\n");
    fprintf(out, "Owner:    %s%s%s\n", owner_name(s->owner), s->owner_name[0] ? ": " : "", s->owner_name);
    switch (s->signal) {
    case SIGNAL_UNKNOWN:
        fprintf(out, "Signal:   unknown (device not opened)\n");
        break;
    case SIGNAL_NONE:
        fprintf(out, "Signal:   none\n");
        break;
    case SIGNAL_ACTIVE:
        mode_text(s, mode, sizeof mode);
        fprintf(out, "Signal:   %s%s\n", mode, s->field_order == FIELD_TOP_FIRST ? ", top field first" : "");
        fprintf(out, "Colour:   %s\n", colour_name(s->colour));
        break;
    }
    if (s->audio_rate)
        fprintf(out, "Audio:    %d Hz, %d channels\n", s->audio_rate, s->audio_channels);
    else
        fprintf(out, "Audio:    none\n");
    fprintf(out, "Parser:   %lu frames, %lu discarded, %lu runts, %lu long lines\n", s->frames, s->discarded,
            s->runts, s->long_lines);
    uint32_t w = hd60s_warnings(s);
    if (!w)
        fprintf(out, "Warnings: none\n");
    for (int i = 0; i < WARNING_COUNT; i++)
        if (w >> i & 1) {
            hd60s_warning_message((enum hd60s_warning)i, s, line, sizeof line);
            fprintf(out, "Warning:  %s\n", line);
        }
}

static void json_str(FILE *out, const char *s)
{
    fputc('"', out);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            fputc('\\', out);
        if ((unsigned char)*s >= 0x20)
            fputc(*s, out);
    }
    fputc('"', out);
}

void hd60s_print_json(const struct hd60s_status *s, FILE *out)
{
    static const char *const state[] = {"unknown", "none", "active"};
    char msg[256];
    fprintf(out, "{\"device\":{\"present\":%s,\"port\":", s->present ? "true" : "false");
    if (s->port_path[0])
        json_str(out, s->port_path);
    else
        fputs("null", out);
    fprintf(out, ",\"speed\":\"%s\",\"access\":\"%s\",\"firmware\":", speed_code(s->speed),
            s->access == ACCESS_OK ? "ok" : s->access == ACCESS_BUSY ? "busy" : s->access == ACCESS_FAILED ? "failed" : "not_tried");
    if (s->fw_valid)
        fprintf(out, "\"%02x.%02x.%02x\"", s->mcu_version >> 16 & 0xff, s->mcu_version >> 8 & 0xff,
                s->mcu_version & 0xff);
    else
        fputs("null", out);
    fprintf(out, ",\"owner\":{\"kind\":\"%s\",\"pid\":", owner_code(s->owner));
    if (s->owner_pid)
        fprintf(out, "%d", s->owner_pid);
    else
        fputs("null", out);
    fputs(",\"name\":", out);
    if (s->owner_name[0])
        json_str(out, s->owner_name);
    else
        fputs("null", out);
    fprintf(out, "}},\"signal\":{\"state\":\"%s\"", state[s->signal]);
    if (s->signal == SIGNAL_ACTIVE)
        fprintf(out,
                ",\"width\":%d,\"height\":%d,\"interlaced\":%s,\"field_order\":\"%s\",\"fps\":%.2f,\"colour\":\"%s\"",
                s->width, s->height, s->interlaced ? "true" : "false",
                s->field_order == FIELD_TOP_FIRST ? "top_first" : "none", s->fps,
                s->colour == COLOUR_BT709 ? "bt709" : s->colour == COLOUR_BT601 ? "bt601" : "unknown");
    fputs("},\"audio\":", out);
    if (s->audio_rate)
        fprintf(out, "{\"rate\":%d,\"channels\":%d}", s->audio_rate, s->audio_channels);
    else
        fputs("null", out);
    fprintf(out, ",\"parser\":{\"frames\":%lu,\"discarded\":%lu,\"runts\":%lu,\"long_lines\":%lu},\"warnings\":[",
            s->frames, s->discarded, s->runts, s->long_lines);
    uint32_t w = hd60s_warnings(s);
    int first = 1;
    for (int i = 0; i < WARNING_COUNT; i++)
        if (w >> i & 1) {
            hd60s_warning_message((enum hd60s_warning)i, s, msg, sizeof msg);
            fprintf(out, "%s{\"code\":\"%s\",\"message\":", first ? "" : ",", hd60s_warning_code((enum hd60s_warning)i));
            json_str(out, msg);
            fputc('}', out);
            first = 0;
        }
    fputs("]}\n", out);
}

#define NO_NTSC UINT_MAX
#define EXACT_FPS_TOLERANCE 0.05

static const struct {
    int lo, hi, tenths;
    unsigned ntsc_from;
} rate_bands[] = {
    {1191, 1209, 1200, 1043},   {991, 1009, 1000, NO_NTSC}, {841, 859, 850, NO_NTSC}, {741, 759, 750, NO_NTSC},
    {711, 729, 720, NO_NTSC},   {691, 708, 700, NO_NTSC},   {591, 609, 600, 2086},    {551, 559, 560, NO_NTSC},
    {491, 509, 500, NO_NTSC},   {241, 259, 250, NO_NTSC},   {236, 240, 240, 5217},    {291, 309, 300, 4174},
};

struct hd60s_rate hd60s_rate_from_vsync(unsigned counter)
{
    if (counter == 0)
        return (struct hd60s_rate){0, 0};
    int raw = (int)(1250000 / counter);
    for (size_t i = 0; i < sizeof rate_bands / sizeof *rate_bands; i++)
        if (raw >= rate_bands[i].lo && raw <= rate_bands[i].hi)
            return (struct hd60s_rate){rate_bands[i].tenths, counter >= rate_bands[i].ntsc_from};
    return (struct hd60s_rate){raw, 0};
}

double hd60s_rate_hz(struct hd60s_rate r)
{
    return r.tenths / 10.0 * (r.ntsc ? 1000.0 / 1001.0 : 1.0);
}

double hd60s_exact_fps(double measured, struct hd60s_rate r, int interlaced)
{
    double exact = hd60s_rate_hz(r) / (interlaced ? 2 : 1);
    return exact > 0 && measured > 0 && fabs(exact - measured) < measured * EXACT_FPS_TOLERANCE ? exact : measured;
}
