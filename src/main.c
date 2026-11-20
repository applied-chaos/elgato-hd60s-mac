#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "device.h"
#include "parser.h"

#define NUM_XFERS 16
#define XFER_SIZE (512 * 1024)
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
    size_t audio_bytes, usb_bytes;
    int active;
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

static void LIBUSB_CALL on_xfer(struct libusb_transfer *t)
{
    struct capture *c = t->user_data;
    if (t->status == LIBUSB_TRANSFER_COMPLETED || t->status == LIBUSB_TRANSFER_TIMED_OUT) {
        c->usb_bytes += (size_t)t->actual_length;
        parser_feed(&c->parser, t->buffer, (size_t)t->actual_length);
    } else if (t->status != LIBUSB_TRANSFER_CANCELLED) {
        fprintf(stderr, "hd60s: transfer status %d\n", t->status);
        stop_requested = 1;
    }
    if (!stop_requested && libusb_submit_transfer(t) == 0)
        return;
    c->active--;
}

/* Streams until want frames, seconds elapse or SIGINT. */
static int stream(struct hd60s *d, struct capture *c, double seconds)
{
    static uint8_t frame[FRAME_CAP];
    struct libusb_transfer *xfers[NUM_XFERS];
    parser_init(&c->parser, frame, sizeof frame, on_frame, on_audio, c);
    if (libusb_set_interface_alt_setting(d->h, 0, 4) < 0) {
        fprintf(stderr, "hd60s: cannot select bulk alt setting\n");
        return -1;
    }
    for (int i = 0; i < NUM_XFERS; i++) {
        xfers[i] = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(xfers[i], d->h, HD60S_EP_VIDEO, malloc(XFER_SIZE), XFER_SIZE,
                                  on_xfer, c, 1000);
        if (libusb_submit_transfer(xfers[i]) == 0)
            c->active++;
    }
    double end = now() + seconds;
    while (c->active) {
        if (stop_requested || (seconds > 0 && now() > end)) {
            stop_requested = 1;
            for (int i = 0; i < NUM_XFERS; i++)
                libusb_cancel_transfer(xfers[i]);
        }
        struct timeval tv = {0, 100000};
        libusb_handle_events_timeout_completed(d->usb, &tv, NULL);
    }
    for (int i = 0; i < NUM_XFERS; i++) {
        free(xfers[i]->buffer);
        libusb_free_transfer(xfers[i]);
    }
    libusb_set_interface_alt_setting(d->h, 0, 0);
    return 0;
}

static void report(const struct capture *c)
{
    double span = c->last - c->first;
    double fps = c->frames > 1 && span > 0 ? (double)(c->frames - 1) / span : 0;
    if (!c->frames)
        fprintf(stderr, "signal: none (%zu bytes received)\n", c->usb_bytes);
    else
        fprintf(stderr, "signal: %dx%d%s %.2f fps, audio %zu bytes, usb %zu bytes\n", c->width,
                c->height, c->interlaced ? "i" : "p", fps, c->audio_bytes, c->usb_bytes);
}

static int usage(void)
{
    fprintf(stderr, "usage: hd60s info\n"
                    "       hd60s capture [-n frames] [-o out.uyvy|-] [-a out.pcm]\n"
                    "video: raw YUY2 (ffmpeg yuyv422); audio: 48 kHz s16le stereo\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage();
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
            stream(&d, &c, 2.0);
        } else {
            stream(&d, &c, 0);
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
