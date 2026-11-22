#include "device_libusb.h"

#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int lctrl(void *ctx, int in, uint8_t req, uint16_t val, uint16_t idx, uint8_t *buf, uint16_t len)
{
    struct hd60s *d = ctx;
    int r = libusb_control_transfer(d->h, in ? 0xc0 : 0x40, req, val, idx, buf, len, 3000);
    /* 0xEC/0xC2/0xC7 are Elgato's init requests, whose failures it ignores. */
    if (r < 0 && !d->err && req != 0xec && req != 0xc2 && req != 0xc7)
        fprintf(stderr, "hd60s: request %02x %04x failed: %s\n", req, val, libusb_error_name(r));
    return r;
}

static void lsleep(void *ctx, int ms)
{
    (void)ctx;
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

int hd60s_quiet;

int hd60s_open(struct hd60s *d)
{
    memset(d, 0, sizeof *d);
    if (libusb_init(&d->lusb) < 0)
        return -1;
    d->open_err = LIBUSB_ERROR_NO_DEVICE;
    libusb_device **list;
    ssize_t n = libusb_get_device_list(d->lusb, &list);
    for (ssize_t i = 0; i < n && !d->h; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) == 0 && dd.idVendor == HD60S_VID && dd.idProduct == HD60S_PID)
            d->open_err = libusb_open(list[i], &d->h);
    }
    if (n >= 0)
        libusb_free_device_list(list, 1);
    if (!d->h) {
        if (!hd60s_quiet)
            fprintf(stderr, "hd60s: no %04x:%04x found (or it is busy)\n", HD60S_VID, HD60S_PID);
        return -1;
    }
    int cfg = 0;
    libusb_get_configuration(d->h, &cfg);
    if (cfg != 1 && (d->open_err = libusb_set_configuration(d->h, 1)) < 0)
        return -1;
    if ((d->open_err = libusb_claim_interface(d->h, 0)) < 0) {
        if (!hd60s_quiet)
            fprintf(stderr, "hd60s: cannot claim interface 0\n");
        return -1;
    }
    d->usb = (struct hd60s_usb){.ctx = d, .ctrl = lctrl, .sleep_ms = lsleep};
    if (hd60s_init(d) < 0) {
        if (!d->err)
            fprintf(stderr, "hd60s: MCU bridge did not answer\n");
        return -1;
    }
    return 0;
}

void hd60s_close(struct hd60s *d)
{
    if (d->h) {
        libusb_release_interface(d->h, 0);
        libusb_close(d->h);
    }
    if (d->lusb)
        libusb_exit(d->lusb);
    memset(d, 0, sizeof *d);
}

#define MAX_XFERS 16
#define XFER_SIZE (512 * 1024)

struct stream_ctx {
    struct hd60s_parser *parser;
    int iso_packet_bytes;
    volatile sig_atomic_t *stop;
    int active;
    volatile int watch_fired, watch_released;
    frame_cb frame_cb;
    void *frame_ctx;
    struct hd60s_policy policy;
};

static void on_frame_probe(void *ctx, const uint8_t *f, int w, int h, int interlaced)
{
    struct stream_ctx *s = ctx;
    hd60s_policy_frame(&s->policy, w, h, interlaced);
    s->frame_cb(s->frame_ctx, f, w, h, interlaced);
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void LIBUSB_CALL on_xfer(struct libusb_transfer *t)
{
    struct stream_ctx *s = t->user_data;
    if (t->status == LIBUSB_TRANSFER_COMPLETED || t->status == LIBUSB_TRANSFER_TIMED_OUT) {
        if (t->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
            for (int i = 0; i < t->num_iso_packets; i++)
                if (t->iso_packet_desc[i].status == LIBUSB_TRANSFER_COMPLETED)
                    parser_feed(s->parser, t->buffer + (size_t)i * s->iso_packet_bytes,
                                t->iso_packet_desc[i].actual_length);
        } else {
            parser_feed(s->parser, t->buffer, (size_t)t->actual_length);
        }
    } else if (t->status != LIBUSB_TRANSFER_CANCELLED) {
        fprintf(stderr, "hd60s: transfer status %d\n", t->status);
        *s->stop = 1;
    }
    if (!*s->stop && libusb_submit_transfer(t) == 0)
        return;
    s->active--;
}

static int watch_enabled(void) { return !getenv("HD60S_NO_WATCH"); }

static void LIBUSB_CALL on_watch(struct libusb_transfer *t)
{
    struct stream_ctx *s = t->user_data;
    if (t->status == LIBUSB_TRANSFER_COMPLETED)
        s->watch_fired = 1;
    else
        s->watch_released = 1;
}

static void iso_alt_bytes(struct hd60s *d, int out[HD60S_ISO_ALTS])
{
    struct libusb_config_descriptor *cfg;
    if (libusb_get_active_config_descriptor(libusb_get_device(d->h), &cfg) < 0)
        return;
    const struct libusb_interface *intf = &cfg->interface[0];
    for (int i = 0; i < intf->num_altsetting; i++) {
        const struct libusb_interface_descriptor *a = &intf->altsetting[i];
        if (a->bAlternateSetting < 1 || a->bAlternateSetting > HD60S_ISO_ALTS)
            continue;
        for (int e = 0; e < a->bNumEndpoints; e++) {
            const struct libusb_endpoint_descriptor *ep = &a->endpoint[e];
            if (ep->bEndpointAddress != HD60S_EP_VIDEO || (ep->bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_ISOCHRONOUS)
                continue;
            struct libusb_ss_endpoint_companion_descriptor *ss;
            int n = (ep->wMaxPacketSize & 0x7ff) * (1 + (ep->wMaxPacketSize >> 11 & 3));
            if (libusb_get_ss_endpoint_companion_descriptor(NULL, ep, &ss) == 0) {
                n = ss->wBytesPerInterval;
                libusb_free_ss_endpoint_companion_descriptor(ss);
            }
            out[a->bAlternateSetting - 1] = n;
        }
    }
    libusb_free_config_descriptor(cfg);
}

int hd60s_stream(struct hd60s *d, struct hd60s_parser *p, volatile sig_atomic_t *stop, double seconds)
{
    struct stream_ctx s = {.parser = p, .stop = stop, .frame_cb = p->on_frame, .frame_ctx = p->ctx};
    struct libusb_transfer *xfers[MAX_XFERS];
    int alt_bytes[HD60S_ISO_ALTS] = {0};
    struct hd60s_xfer_plan plan;
    if (d->usb.transfer == HD60S_ISO)
        iso_alt_bytes(d, alt_bytes);
    if (hd60s_xfer_plan(d->usb.transfer, alt_bytes, &plan) < 0) {
        fprintf(stderr, "hd60s: no isochronous alt setting on this device\n");
        return -1;
    }
    if (libusb_set_interface_alt_setting(d->h, 0, plan.alt) < 0) {
        fprintf(stderr, "hd60s: cannot select alt setting %d\n", plan.alt);
        return -1;
    }
    s.iso_packet_bytes = plan.packet_bytes;
    p->on_frame = on_frame_probe;
    p->ctx = &s;
    hd60s_policy_begin(&s.policy, now_ms());
    for (int i = 0; i < plan.buffers; i++) {
        xfers[i] = libusb_alloc_transfer(plan.packets);
        if (plan.packets) {
            int size = plan.packets * plan.packet_bytes;
            libusb_fill_iso_transfer(xfers[i], d->h, HD60S_EP_VIDEO, malloc((size_t)size), size, plan.packets,
                                     on_xfer, &s, 1000);
            libusb_set_iso_packet_lengths(xfers[i], plan.packet_bytes);
        } else {
            libusb_fill_bulk_transfer(xfers[i], d->h, HD60S_EP_VIDEO, malloc(XFER_SIZE), XFER_SIZE,
                                      on_xfer, &s, 1000);
        }
        if (libusb_submit_transfer(xfers[i]) == 0)
            s.active++;
    }
    struct libusb_transfer *wt = NULL;
    uint8_t wbuf[8];
    int claimed = 0;
    if (watch_enabled() && (claimed = libusb_claim_interface(d->h, HD60S_IF_WATCH) == 0) && hd60s_watch_start(d) == 0 &&
        (wt = libusb_alloc_transfer(0))) {
        libusb_fill_interrupt_transfer(wt, d->h, HD60S_EP_WATCH, wbuf, sizeof wbuf, on_watch, &s, 0);
        if (libusb_submit_transfer(wt) < 0) {
            hd60s_watch_stop(d);
            s.watch_released = 1;
        }
    } else {
        s.watch_released = 1;
    }
    long end = now_ms() + (long)(seconds * 1000);
    while (s.active) {
        if (*stop || (seconds > 0 && now_ms() >= end)) {
            *stop = 1;
            for (int i = 0; i < plan.buffers; i++)
                libusb_cancel_transfer(xfers[i]);
        }
        struct timeval tv = {0, 100000};
        libusb_handle_events_timeout_completed(d->lusb, &tv, NULL);
        /* Not from the transfer callback: the sync control transfers need the event loop. */
        if (!*stop) {
            if (s.watch_fired && !s.watch_released) {
                s.watch_fired = 0;
                hd60s_policy_wake(&s.policy, now_ms());
                if (libusb_submit_transfer(wt) < 0)
                    s.watch_released = 1;
            }
            if (s.watch_released && d->watching)
                hd60s_watch_stop(d);
            hd60s_policy_tick(d, p, &s.policy, now_ms());
        }
    }
    hd60s_watch_stop(d);
    if (wt) {
        libusb_cancel_transfer(wt);
        while (!s.watch_released && !s.watch_fired) {
            struct timeval tv = {0, 100000};
            libusb_handle_events_timeout_completed(d->lusb, &tv, NULL);
        }
        libusb_free_transfer(wt);
    }
    if (claimed)
        libusb_release_interface(d->h, HD60S_IF_WATCH);
    p->on_frame = s.frame_cb;
    p->ctx = s.frame_ctx;
    for (int i = 0; i < plan.buffers; i++) {
        free(xfers[i]->buffer);
        libusb_free_transfer(xfers[i]);
    }
    libusb_set_interface_alt_setting(d->h, 0, 0);
    return 0;
}
