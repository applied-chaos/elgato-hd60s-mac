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
                    "       hd60s status [--json]\n"
                    "       hd60s capture [-n frames] [-o out.uyvy|-] [-a out.pcm] [-s hdmi|line|mix] [-g dB] [-m]\n"
                    "video: raw YUY2 (ffmpeg yuyv422); audio: 48 kHz s16le stereo\n"
                    "audio: -s source (default hdmi), -g gain -12..12 dB in 0.5 steps (default -6), -m mute\n");
    return 2;
}

struct watch {
    struct hd60s_status st;
    double first, last;
    unsigned long standard;
    int unknown_w, unknown_h, unknown_interlaced;
    unsigned long unknown_run;            /* same-size run: a real timing, not mode-switch runts */
    size_t audio_bytes;
};

#define UNSUPPORTED_STABLE_FRAMES 15

static void watch_frame(void *ctx, const uint8_t *frame, int w, int h, int interlaced)
{
    struct watch *k = ctx;
    (void)frame;
    if (!hd60s_size_standard(w, h)) {
        k->st.runts++;
        k->unknown_run = w == k->unknown_w && h == k->unknown_h ? k->unknown_run + 1 : 1;
        k->unknown_w = w;
        k->unknown_h = h;
        k->unknown_interlaced = interlaced;
        return;
    }
    k->unknown_run = 0;
    k->last = now();
    if (!k->standard++)
        k->first = k->last;
    k->st.width = w;
    k->st.height = h;
    k->st.interlaced = interlaced;
}

static void watch_audio(void *ctx, const uint8_t *pcm, size_t len)
{
    struct watch *k = ctx;
    (void)pcm;
    k->audio_bytes += len;
}

/* Reports without failing: a busy or missing device is a result, not an error. */
static int status(int json)
{
    static uint8_t frame[FRAME_CAP];
    struct watch k = {0};
    struct hd60s_status *st = &k.st;
    hd60s_probe_usb(st);
    if (st->present) {
        struct hd60s d;
        struct hd60s_parser parser;
        hd60s_quiet = 1;
        int opened = hd60s_open(&d) == 0;
        st->access = hd60s_access_from_error(d.open_err);
        if (opened && hd60s_start(&d) == 0) {
            st->owner = OWNER_CLI;
            st->owner_pid = 0;
            st->owner_name[0] = 0;
            st->fw_valid = 1;
            st->mcu_version = d.mcu_version;
            parser_init(&parser, frame, sizeof frame, watch_frame, watch_audio, &k);
            hd60s_stream(&d, &parser, &stop_requested, 2.0);
            st->signal = k.standard ? SIGNAL_ACTIVE : SIGNAL_NONE;
            if (k.unknown_run >= UNSUPPORTED_STABLE_FRAMES) {
                st->signal = SIGNAL_UNSUPPORTED;
                st->width = k.unknown_w;
                st->height = k.unknown_h;
                st->interlaced = k.unknown_interlaced;
            }
            st->frames = parser.frames;
            st->discarded = parser.discarded;
            st->long_lines = parser.long_total;
        } else if (opened) {
            st->access = ACCESS_FAILED;
        }
        if (st->signal == SIGNAL_ACTIVE) {
            double span = k.last - k.first;
            st->fps = k.standard > 1 && span > 0 ? (double)(k.standard - 1) / span : 0;
            st->fps = hd60s_exact_fps(st->fps, hd60s_rate_from_vsync(hd60s_vsync_counter(&d)), st->interlaced);
            st->field_order = st->interlaced ? FIELD_TOP_FIRST : FIELD_NONE;
            st->colour = parser.colour;
            st->input_format = parser.input_format;
        }
        if (k.audio_bytes) {
            st->audio_rate = 48000;
            st->audio_channels = 2;
        }
        hd60s_stop(&d);
        hd60s_close(&d);
    }
    if (json)
        hd60s_print_json(st, stdout);
    else
        hd60s_print_text(st, stdout);
    return 0;
}

int main(int argc, char **argv)
{
    static uint8_t frame[FRAME_CAP];
    if (argc < 2)
        return usage();
    if (strcmp(argv[1], "status") == 0)
        return status(argc > 2 && strcmp(argv[2], "--json") == 0);
    int info = strcmp(argv[1], "info") == 0;
    if (!info && strcmp(argv[1], "capture") != 0)
        return usage();

    struct capture c = {0};
    const char *vpath = NULL, *apath = NULL;
    struct audio_settings audio = AUDIO_DEFAULT;
    int opt;
    optind = 2;
    while ((opt = getopt(argc, argv, "n:o:a:s:g:m")) != -1) {
        if (opt == 'n')
            c.want = strtol(optarg, NULL, 10);
        else if (opt == 'o')
            vpath = optarg;
        else if (opt == 'a')
            apath = optarg;
        else if (opt == 's') {
            if (!audio_source_parse(optarg, &audio.source))
                return usage();
        } else if (opt == 'g') {
            double db = strtod(optarg, NULL);
            if (db < AUDIO_GAIN_MIN / 2.0 || db > AUDIO_GAIN_MAX / 2.0)
                return usage();
            audio.gain_half_db = (int)lround(db * 2);
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
    if (hd60s_open(&d) == 0 && hd60s_set_audio(&d, &audio) == 0 && hd60s_start(&d) == 0) {
        if (info) {
            fprintf(stderr, "mcu firmware: %02x.%02x.%02x\n", d.mcu_version >> 16 & 0xff,
                    d.mcu_version >> 8 & 0xff, d.mcu_version & 0xff);
            parser_init(&c.parser, frame, sizeof frame, on_frame, on_audio, &c);
            hd60s_stream(&d, &c.parser, &stop_requested, 2.0);
        } else {
            parser_init(&c.parser, frame, sizeof frame, on_frame, on_audio, &c);
            hd60s_stream(&d, &c.parser, &stop_requested, 0);
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
