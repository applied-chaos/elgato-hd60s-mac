#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "device_libusb.h"
#include "parser.h"
#include "status.h"

#define FRAME_CAP (1920 * 1440 * 2)

static volatile sig_atomic_t stop_requested;
static void on_sigint(int sig)
{
    (void)sig;
    stop_requested = 1;
}

struct capture {
    struct hd60s_parser parser;
    FILE *video, *audio;
    long frames, want;
    int width, height, interlaced;
    double first, last;
    size_t audio_bytes;
};

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void on_frame(void *ctx, const uint8_t *frame, int w, int h, int interlaced)
{
    struct capture *c = ctx;
    if (c->want && c->frames >= c->want)
        return;
    if (c->frames && (w != c->width || h != c->height))
        fprintf(stderr, "hd60s: format changed to %dx%d\n", w, h);
    c->width = w;
    c->height = h;
    c->interlaced = interlaced;
    c->last = now();
    if (!c->frames)
        c->first = c->last;
    if (c->video && fwrite(frame, (size_t)w * 2, (size_t)h, c->video) != (size_t)h)
        stop_requested = 1;
    if (++c->frames == c->want)
        stop_requested = 1;
}

static void on_audio(void *ctx, const uint8_t *pcm, size_t len)
{
    struct capture *c = ctx;
    c->audio_bytes += len;
    if (c->audio)
        fwrite(pcm, 1, len, c->audio);
}

static void report(const struct capture *c)
{
    double span = c->last - c->first;
    double fps = c->frames > 1 && span > 0 ? (double)(c->frames - 1) / span : 0;
    if (!c->frames)
        fprintf(stderr, "signal: none\n");
    else
        fprintf(stderr, "signal: %dx%d%s %.2f fps, audio %zu bytes\n", c->width,
                c->height, c->interlaced ? "i" : "p", fps, c->audio_bytes);
    fprintf(stderr, "parser: %lu frames, %lu discarded, %lu long lines kept\n", c->parser.frames,
            c->parser.discarded, c->parser.long_total);
}

static int usage(void)
{
    fprintf(stderr, "usage: hd60s info\n"
                    "       hd60s status [--json] [-r range] [picture]\n"
                    "       hd60s capture [-n frames] [-o out.uyvy|-] [-a out.pcm] [-s hdmi|line|mix] [-g dB] [-m] [-r range] [--iso] [picture]\n"
                    "video: raw YUY2 (ffmpeg yuyv422); audio: 48 kHz s16le stereo\n"
                    "transfer: bulk by default, --iso for isochronous (flaky hubs)\n"
                    "audio: -s source (default hdmi), -g gain -12..12 dB in 0.5 steps (default -6), -m mute\n"
                    "range: auto (default), standard, expanded (source is full-range RGB)\n"
                    "picture: --brightness N, --contrast N, --saturation N, --hue N, each -128..127 from Elgato's default (0)\n");
    return 2;
}

/* Takes --brightness/--contrast/--saturation/--hue N out of argv. 0, or -1 on a bad value. */
static int picture_args(int *argc, char **argv, struct hd60s_picture *pic)
{
    static const char *const names[] = {"--brightness", "--contrast", "--saturation", "--hue"};
    int8_t *const fields[] = {&pic->brightness, &pic->contrast, &pic->saturation, &pic->hue};
    for (int i = 2; i < *argc; i++)
        for (int k = 0; k < 4; k++) {
            if (strcmp(argv[i], names[k]) != 0)
                continue;
            char *end;
            long v = i + 1 < *argc ? strtol(argv[i + 1], &end, 10) : 0;
            if (i + 1 >= *argc || *end || v < -128 || v > 127)
                return -1;
            *fields[k] = (int8_t)v;
            memmove(&argv[i], &argv[i + 2], (size_t)(*argc - i - 1) * sizeof *argv);
            *argc -= 2;
            i--;
            break;
        }
    return 0;
}

/* Reports without failing: a busy or missing device is a result, not an error. */
static int status(int json, struct hd60s_settings settings)
{
    struct hd60s_status st;
    hd60s_sample(&st, 2.0, settings, &stop_requested);
    if (json)
        hd60s_print_json(&st, stdout);
    else
        hd60s_print_text(&st, stdout);
    return 0;
}

int main(int argc, char **argv)
{
    static uint8_t frame[FRAME_CAP];
    if (argc < 2)
        return usage();
    struct hd60s_settings settings = {RANGE_AUTO, {0}, {0}};
    if (picture_args(&argc, argv, &settings.picture) < 0)
        return usage();
    if (strcmp(argv[1], "status") == 0) {
        int json = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--json") == 0)
                json = 1;
            else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc && hd60s_range_from_name(argv[i + 1]) >= 0)
                settings.range = (enum hd60s_range)hd60s_range_from_name(argv[++i]);
            else
                return usage();
        }
        return status(json, settings);
    }
    int info = strcmp(argv[1], "info") == 0;
    if (!info && strcmp(argv[1], "capture") != 0)
        return usage();

    struct capture c = {0};
    const char *vpath = NULL, *apath = NULL;
    struct audio_settings audio = AUDIO_DEFAULT;
    int opt;
    enum hd60s_transfer transfer = HD60S_BULK;
    for (int i = 2; i < argc; i++)
        if (strcmp(argv[i], "--iso") == 0) {
            transfer = HD60S_ISO;
            memmove(&argv[i], &argv[i + 1], (size_t)(argc - i) * sizeof *argv);
            argc--;
            i--;
        }
    optind = 2;
    while ((opt = getopt(argc, argv, "n:o:a:s:g:mr:")) != -1) {
        if (opt == 'n')
            c.want = strtol(optarg, NULL, 10);
        else if (opt == 'o')
            vpath = optarg;
        else if (opt == 'a')
            apath = optarg;
        else if (opt == 'r' && hd60s_range_from_name(optarg) >= 0)
            settings.range = (enum hd60s_range)hd60s_range_from_name(optarg);
        else if (opt == 's') {
            if (!audio_source_parse(optarg, &audio.source))
                return usage();
        } else if (opt == 'g') {
            double db = strtod(optarg, NULL);
            if (db < audio_gain_to_db(AUDIO_GAIN_MIN) || db > audio_gain_to_db(AUDIO_GAIN_MAX))
                return usage();
            audio.gain_half_db = audio_gain_from_db(db);
        } else if (opt == 'm')
            audio.mute = 1;
        else
            return usage();
    }
    if (vpath)
        c.video = strcmp(vpath, "-") == 0 ? stdout : fopen(vpath, "wb");
    if (apath)
        c.audio = fopen(apath, "wb");
    if ((vpath && !c.video) || (apath && !c.audio)) {
        perror("hd60s");
        return 1;
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGPIPE, SIG_IGN); /* a closed reader becomes a write error, so the device is still stopped cleanly */
    struct hd60s d;
    int rc = 1;
    hd60s_open_wait_ms = 4000;
    if (hd60s_open(&d) == 0 && (settings.audio = audio_to_pref(audio), hd60s_settings_publish(&d, &settings), hd60s_set_audio(&d, &audio) == 0) && hd60s_start(&d) == 0) {
        d.usb.transfer = transfer;
        if (info) {
            fprintf(stderr, "mcu firmware: %02x.%02x.%02x\n", d.mcu_version >> 16 & 0xff,
                    d.mcu_version >> 8 & 0xff, d.mcu_version & 0xff);
            parser_init(&c.parser, frame, sizeof frame, on_frame, on_audio, &c);
            hd60s_stream(&d, &c.parser, &stop_requested, NULL, 2.0);
        } else {
            parser_init(&c.parser, frame, sizeof frame, on_frame, on_audio, &c);
            hd60s_stream(&d, &c.parser, &stop_requested, NULL, 0);
        }
        report(&c);
        rc = info || c.frames ? 0 : 1;
    }
    hd60s_stop(&d);
    hd60s_close(&d);
    if (c.video && c.video != stdout)
        fclose(c.video);
    if (c.audio)
        fclose(c.audio);
    return rc;
}
