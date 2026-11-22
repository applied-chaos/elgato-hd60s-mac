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
                    "       hd60s capture [-n frames] [-o out.uyvy|-] [-a out.pcm]\n"
                    "video: raw YUY2 (ffmpeg yuyv422); audio: 48 kHz s16le stereo\n");
    return 2;
}

/* Reports without failing: a busy or missing device is a result, not an error. */
static int status(int json)
{
    struct hd60s_status st;
    hd60s_sample(&st, 2.0, &stop_requested);
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
    if (strcmp(argv[1], "status") == 0)
        return status(argc > 2 && strcmp(argv[2], "--json") == 0);
    int info = strcmp(argv[1], "info") == 0;
    if (!info && strcmp(argv[1], "capture") != 0)
        return usage();

    struct capture c = {0};
    const char *vpath = NULL, *apath = NULL;
    int opt;
    optind = 2;
    while ((opt = getopt(argc, argv, "n:o:a:")) != -1) {
        if (opt == 'n')
            c.want = strtol(optarg, NULL, 10);
        else if (opt == 'o')
            vpath = optarg;
        else if (opt == 'a')
            apath = optarg;
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
    if (hd60s_open(&d) == 0 && hd60s_start(&d) == 0) {
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
