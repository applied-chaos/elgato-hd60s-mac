#include <stdlib.h>
#include <time.h>

#include "device_libusb.h"
#include "status.h"
#include "tx.h"

#define FRAME_CAP (1920 * 1440 * 2)
#define UNSUPPORTED_STABLE_FRAMES 15

struct watch {
    struct hd60s_status st;
    struct hd60s_parser *parser;
    double first, last;
    unsigned long standard;
    int unknown_w, unknown_h, unknown_interlaced;
    unsigned long unknown_run;
    size_t audio_bytes;
};

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

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
    k->st.field_order = k->parser->top_first ? FIELD_TOP_FIRST : FIELD_NONE;
}

static void watch_audio(void *ctx, const uint8_t *pcm, size_t len)
{
    struct watch *k = ctx;
    (void)pcm;
    k->audio_bytes += len;
}

/* Reports without failing: a busy or missing device is a result, not an error. */
void hd60s_sample(struct hd60s_status *out, double seconds, struct hd60s_settings settings, volatile sig_atomic_t *stop)
{
    uint8_t *frame = malloc(FRAME_CAP);
    struct watch k = {0};
    struct hd60s_status *st = &k.st;
    hd60s_probe_usb(st);
    st->range = settings.range;
    if (st->present && frame) {
        struct hd60s d;
        struct hd60s_parser parser;
        hd60s_quiet = 1;
        int opened = hd60s_open(&d) == 0;
        st->access = hd60s_access_from_error(d.open_err);
        d.settings = settings;
        if (opened) {
            struct audio_settings audio = audio_from_pref(settings.audio);
            (void)hd60s_set_audio(&d, &audio);
        }
        if (opened && hd60s_start(&d) == 0) {
            st->owner = OWNER_CLI;
            st->owner_pid = 0;
            st->owner_name[0] = 0;
            st->fw_valid = 1;
            st->mcu_version = d.mcu_version;
            k.parser = &parser;
            parser_init(&parser, frame, FRAME_CAP, watch_frame, watch_audio, &k);
            hd60s_stream(&d, &parser, stop, seconds);
            st->signal = k.standard ? SIGNAL_ACTIVE : SIGNAL_NONE;
            if (k.unknown_run >= UNSUPPORTED_STABLE_FRAMES) {
                st->signal = SIGNAL_UNSUPPORTED;
                st->width = k.unknown_w;
                st->height = k.unknown_h;
                st->interlaced = k.unknown_interlaced;
            }
            struct hd60s_link link = hd60s_read_link(&d);
            if (link.hdcp)
                st->signal = SIGNAL_HDCP;
            st->source = hd60s_link_type_from_link(link, st->signal);
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
            st->colour = parser.colour;
            st->input_format = parser.input_format;
        }
        if (k.audio_bytes) {
            st->audio_rate = 48000;
            st->audio_channels = 2;
            st->audio_source_rate =
                hd60s_source_audio_hz(&d, tx_mode_find(st->width, st->height, st->interlaced, st->interlaced ? st->fps * 2 : st->fps));
        }
        hd60s_stop(&d);
        hd60s_close(&d);
    }
    free(frame);
    *out = *st;
}
