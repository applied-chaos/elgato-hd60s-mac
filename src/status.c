#include "status.h"

#include "parser.h"

#include <libusb.h>
#include <limits.h>
#include <math.h>
#include <string.h>

struct named {
    int value;
    const char *code;
    const char *text;
};

#define COUNT_OF(t) (sizeof(t) / sizeof *(t))
#define NAME_OF(t, v) named((t), COUNT_OF(t), (v))

static const struct named *named(const struct named *t, size_t n, int v)
{
    for (size_t i = 0; i < n; i++)
        if (t[i].value == v)
            return &t[i];
    return t;
}

static const struct named speeds[] = {
    {SPEED_UNKNOWN, "unknown", "unknown speed"},
    {SPEED_LOW, "low", "USB 1.0 low speed (1.5 Mb/s)"},
    {SPEED_FULL, "full", "USB 1.1 full speed (12 Mb/s)"},
    {SPEED_HIGH, "high", "USB 2.0 high speed (480 Mb/s)"},
    {SPEED_SUPER, "super", "USB 3.0 SuperSpeed (5 Gb/s)"},
    {SPEED_SUPER_PLUS, "super_plus", "USB 3.1+ SuperSpeed+ (10 Gb/s or more)"},
};
static const struct named accesses[] = {
    {ACCESS_NOT_TRIED, "not_tried", ""}, {ACCESS_OK, "ok", ""}, {ACCESS_BUSY, "busy", ""}, {ACCESS_FAILED, "failed", ""},
};
static const struct named owners[] = {
    {OWNER_UNKNOWN, "unknown", "unknown"},
    {OWNER_NONE, "nobody", "nobody"},
    {OWNER_CLI, "cli", "this tool"},
    {OWNER_CAMERA, "camera_extension", "camera extension"},
    {OWNER_DRIVER, "driver_extension", "driver extension"},
    {OWNER_OTHER, "other", "another process"},
};
static const struct named signals[] = {
    {SIGNAL_UNKNOWN, "unknown", ""}, {SIGNAL_NONE, "none", ""}, {SIGNAL_ACTIVE, "active", ""},
    {SIGNAL_UNSUPPORTED, "unsupported", ""}, {SIGNAL_HDCP, "hdcp", ""},
};
static const struct named sources[] = {
    {SOURCE_UNKNOWN, "unknown", "unknown"}, {SOURCE_HDMI, "hdmi", "HDMI"}, {SOURCE_DVI, "dvi", "DVI"},
};
static const struct named field_orders[] = {{FIELD_NONE, "none", ""}, {FIELD_TOP_FIRST, "top_first", ""}};
static const struct named colours[] = {
    {COLOUR_UNKNOWN, "unknown", "unknown"}, {COLOUR_BT709, "bt709", "BT.709"}, {COLOUR_BT601, "bt601", "BT.601"},
};
static const struct named inputs[] = {
    {INPUT_RGB, "rgb", "RGB"}, {INPUT_YCBCR444, "ycbcr444", "YCbCr 4:4:4"}, {INPUT_YCBCR422, "ycbcr422", "YCbCr 4:2:2"},
};
struct warning_args {
    const char *speed, *at, *owner, *owner_sep, *owner_name;
};
static const struct named warnings[] = {
    {WARN_NO_DEVICE, "no_device", "No HD60 S found. Check the USB cable and that the device is powered."},
    {WARN_NOT_SUPERSPEED, "not_superspeed",
     "Connected at %1$s%2$s, not SuperSpeed. Use a USB 3 port, a USB 3 cable, and no USB 2 hub."},
    {WARN_BUSY, "busy", "Device is in use by %3$s%4$s%5$s. Signal details unavailable until it lets go."},
    {WARN_OPEN_FAILED, "open_failed", "Could not open the device. Unplug and replug it."},
};

_Static_assert(COUNT_OF(speeds) == SPEED_COUNT, "every speeds value needs a name in its table");
_Static_assert(COUNT_OF(accesses) == ACCESS_COUNT, "every accesses value needs a name in its table");
_Static_assert(COUNT_OF(owners) == OWNER_COUNT, "every owners value needs a name in its table");
_Static_assert(COUNT_OF(signals) == SIGNAL_COUNT, "every signals value needs a name in its table");
_Static_assert(COUNT_OF(sources) == SOURCE_COUNT, "every sources value needs a name in its table");
_Static_assert(COUNT_OF(field_orders) == FIELD_COUNT, "every field orders value needs a name in its table");
_Static_assert(COUNT_OF(colours) == COLOUR_COUNT, "every colours value needs a name in its table");
_Static_assert(COUNT_OF(inputs) == INPUT_COUNT, "every inputs value needs a name in its table");
_Static_assert(COUNT_OF(warnings) == WARN_COUNT, "every warnings value needs a name in its table");
_Static_assert(WARN_COUNT <= 32, "warnings are bits of a uint32_t");

static const uint32_t KNOWN_MCU_VERSIONS[] = {HD60S_TESTED_MCU};

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

/* A kernel or dext client names its service class instead of a pid. */
enum hd60s_owner hd60s_owner_from_service(const char *s)
{
    return strcmp(s, "HD60SDriver") == 0 ? OWNER_DRIVER : OWNER_OTHER;
}

int hd60s_size_standard(int w, int h)
{
    return parser_is_sd(w, h) >= 0;
}

enum hd60s_signal hd60s_ring_signal(int advanced, int hdcp, int w, int h, int prev_w, int prev_h)
{
    if (hdcp)
        return SIGNAL_HDCP;
    if (!advanced)
        return SIGNAL_NONE;
    if (hd60s_size_standard(w, h))
        return SIGNAL_ACTIVE;
    return w > 0 && h > 0 && w == prev_w && h == prev_h ? SIGNAL_UNSUPPORTED : SIGNAL_NONE;
}

static int fw_known(uint32_t v)
{
    for (size_t i = 0; i < COUNT_OF(KNOWN_MCU_VERSIONS); i++)
        if (KNOWN_MCU_VERSIONS[i] == v)
            return 1;
    return 0;
}

int hd60s_firmware_untested(const struct hd60s_status *s)
{
    return s->fw_valid && !fw_known(s->mcu_version);
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
    return w;
}

const char *hd60s_warning_code(enum hd60s_warning w)
{
    return NAME_OF(warnings, w)->code;
}

void hd60s_warning_message(enum hd60s_warning w, const struct hd60s_status *s, char *buf, size_t n)
{
    char at[64] = "";
    if (s->port_path[0])
        snprintf(at, sizeof at, " (port %s)", s->port_path);
    const struct warning_args a = {NAME_OF(speeds, s->speed)->text, at, NAME_OF(owners, s->owner)->text,
                                   s->owner_name[0] ? ": " : "", s->owner_name};
    snprintf(buf, n, NAME_OF(warnings, w)->text, a.speed, a.at, a.owner, a.owner_sep, a.owner_name);
}

static void untested_message(const struct hd60s_status *s, char *buf, size_t n)
{
    snprintf(buf, n, "Untested firmware %02x.%02x.%02x.", s->mcu_version >> 16 & 0xff, s->mcu_version >> 8 & 0xff,
             s->mcu_version & 0xff);
}

enum hd60s_link_type hd60s_link_type_from_link(struct hd60s_link l, enum hd60s_signal signal)
{
    return signal == SIGNAL_ACTIVE || signal == SIGNAL_HDCP ? (l.dvi ? SOURCE_DVI : SOURCE_HDMI) : SOURCE_UNKNOWN;
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
        fprintf(out, ", %s%s%s", NAME_OF(speeds, s->speed)->text, s->port_path[0] ? ", port " : "", s->port_path);
    fputc('\n', out);
    if (s->fw_valid)
        fprintf(out, "Firmware: MCU %02x.%02x.%02x\n", s->mcu_version >> 16 & 0xff,
                s->mcu_version >> 8 & 0xff, s->mcu_version & 0xff);
    else
        fprintf(out, "Firmware: unknown\n");
    fprintf(out, "Owner:    %s%s%s\n", NAME_OF(owners, s->owner)->text, s->owner_name[0] ? ": " : "", s->owner_name);
    switch (s->signal) {
    case SIGNAL_UNKNOWN:
    case SIGNAL_COUNT:
        fprintf(out, "Signal:   unknown (device not opened)\n");
        break;
    case SIGNAL_NONE:
        fprintf(out, "Signal:   none\n");
        break;
    case SIGNAL_UNSUPPORTED:
        fprintf(out, "Signal:   %dx%d%s, unsupported mode\n", s->width, s->height, s->interlaced ? "i" : "p");
        break;
    case SIGNAL_HDCP:
        fprintf(out, "Signal:   HDCP protected\n");
        break;
    case SIGNAL_ACTIVE:
        mode_text(s, mode, sizeof mode);
        fprintf(out, "Signal:   %s%s\n", mode, s->field_order == FIELD_NONE ? "" : ", top field first");
        fprintf(out, "Input:    %s\n", NAME_OF(inputs, s->input_format)->text);
        fprintf(out, "Colour:   %s\n", NAME_OF(colours, s->colour)->text);
        fprintf(out, "Range:    %s\n", hd60s_range_names[s->range]);
        break;
    }
    if (s->source != SOURCE_UNKNOWN)
        fprintf(out, "Source:   %s\n", NAME_OF(sources, s->source)->text);
    if (s->audio_rate) {
        fprintf(out, "Audio:    %d Hz, %d channels", s->audio_rate, s->audio_channels);
        if (s->audio_source_rate)
            fprintf(out, " (source %d Hz)", s->audio_source_rate);
        fputc('\n', out);
    } else
        fprintf(out, "Audio:    none\n");
    fprintf(out, "Parser:   %lu frames, %lu discarded, %lu runts, %lu long lines\n", s->frames, s->discarded,
            s->runts, s->long_lines);
    uint32_t w = hd60s_warnings(s);
    if (!w)
        fprintf(out, "Warnings: none\n");
    if (hd60s_firmware_untested(s)) {
        untested_message(s, line, sizeof line);
        fprintf(out, "Note:     %s\n", line);
    }
    for (int i = 0; i < WARN_COUNT; i++)
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
    char msg[256];
    fprintf(out, "{\"device\":{\"present\":%s,\"port\":", s->present ? "true" : "false");
    if (s->port_path[0])
        json_str(out, s->port_path);
    else
        fputs("null", out);
    fprintf(out, ",\"speed\":\"%s\",\"access\":\"%s\",\"firmware\":", NAME_OF(speeds, s->speed)->code,
            NAME_OF(accesses, s->access)->code);
    if (s->fw_valid)
        fprintf(out, "\"%02x.%02x.%02x\"", s->mcu_version >> 16 & 0xff, s->mcu_version >> 8 & 0xff,
                s->mcu_version & 0xff);
    else
        fputs("null", out);
    fprintf(out, ",\"owner\":{\"kind\":\"%s\",\"pid\":", NAME_OF(owners, s->owner)->code);
    if (s->owner_pid)
        fprintf(out, "%d", s->owner_pid);
    else
        fputs("null", out);
    fputs(",\"name\":", out);
    if (s->owner_name[0])
        json_str(out, s->owner_name);
    else
        fputs("null", out);
    fprintf(out, "}},\"signal\":{\"state\":\"%s\"", NAME_OF(signals, s->signal)->code);
    if (s->signal == SIGNAL_ACTIVE)
        fprintf(out,
                ",\"width\":%d,\"height\":%d,\"interlaced\":%s,\"field_order\":\"%s\",\"fps\":%.2f,\"input\":\"%s\",\"colour\":\"%s\",\"range\":\"%s\"",
                s->width, s->height, s->interlaced ? "true" : "false",
                NAME_OF(field_orders, s->field_order)->code, s->fps, NAME_OF(inputs, s->input_format)->code,
                NAME_OF(colours, s->colour)->code,
                hd60s_range_names[s->range]);
    if (s->source != SOURCE_UNKNOWN)
        fprintf(out, ",\"source\":\"%s\"", NAME_OF(sources, s->source)->code);
    if (s->signal == SIGNAL_UNSUPPORTED)
        fprintf(out, ",\"width\":%d,\"height\":%d", s->width, s->height);
    fputs("},\"audio\":", out);
    if (s->audio_rate) {
        fprintf(out, "{\"rate\":%d,\"channels\":%d,\"source_rate\":", s->audio_rate, s->audio_channels);
        if (s->audio_source_rate)
            fprintf(out, "%d}", s->audio_source_rate);
        else
            fputs("null}", out);
    } else
        fputs("null", out);
    fprintf(out, ",\"parser\":{\"frames\":%lu,\"discarded\":%lu,\"runts\":%lu,\"long_lines\":%lu},\"warnings\":[",
            s->frames, s->discarded, s->runts, s->long_lines);
    uint32_t w = hd60s_warnings(s);
    int first = 1;
    for (int i = 0; i < WARN_COUNT; i++)
        if (w >> i & 1) {
            hd60s_warning_message((enum hd60s_warning)i, s, msg, sizeof msg);
            fprintf(out, "%s{\"code\":\"%s\",\"message\":", first ? "" : ",", hd60s_warning_code((enum hd60s_warning)i));
            json_str(out, msg);
            fputc('}', out);
            first = 0;
        }
    fputs("],\"notes\":[", out);
    if (hd60s_firmware_untested(s)) {
        untested_message(s, msg, sizeof msg);
        fputs("{\"code\":\"untested_firmware\",\"message\":", out);
        json_str(out, msg);
        fputc('}', out);
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
