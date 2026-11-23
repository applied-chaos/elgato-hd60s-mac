#include "../src/framering.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t frame[64 * 4 * 2], out[FR_SLOT_BYTES];

static void fill(uint8_t v)
{
    memset(frame, v, sizeof frame);
}

static void publish_then_take_newest_only(struct fr_header *h)
{
    struct fr_frame info;
    assert(fr_take(h, 0, out, sizeof out, &info) == 0);
    fill(1);
    fr_publish(h, frame, 64, 4, 0, 1, 2);
    fill(2);
    fr_publish(h, frame, 64, 4, 1, 0, 1);
    uint32_t seq = fr_take(h, 0, out, sizeof out, &info);
    assert(seq == 2 && out[0] == 2 && out[sizeof frame - 1] == 2);
    assert(info.width == 64 && info.height == 4 && info.interlaced == 1 && info.wide == 0 && info.colour == 1 && info.len == sizeof frame);
    assert(fr_take(h, seq, out, sizeof out, &info) == 0);
    struct fr_frame head = {0};
    assert(fr_latest(h, &head) == seq && head.width == 64 && head.height == 4 && head.interlaced == 1);
}

static void rejects_torn_and_oversized(struct fr_header *h)
{
    struct fr_frame info;
    fill(3);
    fr_publish(h, frame, 64, 4, 0, 0, 0);
    uint32_t seq = atomic_load(&h->latest);
    atomic_store(&h->slot[seq % FR_SLOTS].seq, 0);
    assert(fr_take(h, 0, out, sizeof out, &info) == 0);
    fr_publish(h, frame, 64, 4, 0, 0, 0);
    assert(fr_take(h, 0, out, 100, &info) == 0);
    uint32_t before = atomic_load(&h->latest);
    fr_publish(h, frame, 4000, 4000, 0, 0, 0);
    assert(atomic_load(&h->latest) == before);
}

static void rejects_a_header_from_another_layout(void)
{
    static _Alignas(64) uint8_t mem[sizeof(struct fr_header)];
    struct fr_header *h = (struct fr_header *)mem;
    memset(mem, 0, sizeof mem);
    assert(!fr_valid(h, sizeof mem));
    fr_init(h);
    assert(!fr_valid(h, sizeof mem));
}

static void status_reaches_the_reader(struct fr_header *h)
{
    fr_status(h, LINK_UP, 10, 2, 3, 1);
    struct fr_counters c = fr_counters(h);
    assert(c.link == LINK_UP && c.frames == 10 && c.discarded == 2 && c.long_lines == 3 && c.errors == 1);
    fr_device(h, 0x110311, 1, 1, 0);
    c = fr_counters(h);
    assert(c.mcu_version == 0x110311 && c.top_first == 1 && c.frames == 10 && c.hdcp == 1 && c.dvi == 0);
    fr_device(h, 0x110311, 1, 0, 1);
    c = fr_counters(h);
    assert(c.hdcp == 0 && c.dvi == 1);
}

struct stress {
    struct fr_header *h;
    _Atomic int done;
};

static void *writer(void *arg)
{
    struct stress *s = arg;
    uint8_t f[64 * 4 * 2];
    for (int n = 1; n <= 20000; n++) {
        memset(f, n & 0xff, sizeof f);
        fr_publish(s->h, f, 64, 4, 0, 0, 0);
    }
    atomic_store(&s->done, 1);
    return NULL;
}

static void concurrent_reader_never_sees_a_torn_frame(struct fr_header *h)
{
    struct stress s = {h, 0};
    pthread_t t;
    pthread_create(&t, NULL, writer, &s);
    uint32_t seen = 0;
    int got = 0;
    struct fr_frame info;
    while (!atomic_load(&s.done)) {
        uint32_t seq = fr_take(h, seen, out, sizeof out, &info);
        if (!seq)
            continue;
        assert(seq > seen);
        for (size_t i = 0; i < info.len; i++)
            assert(out[i] == out[0]);
        seen = seq;
        got++;
    }
    pthread_join(t, NULL);
    assert(got > 0);
}

int main(void)
{
    void *mem = aligned_alloc(4096, fr_bytes());
    struct fr_header *h = fr_init(mem);
    assert(fr_valid(h, fr_bytes()));
    assert(atomic_load(&h->link) == LINK_DOWN);
    publish_then_take_newest_only(h);
    rejects_torn_and_oversized(h);
    rejects_a_header_from_another_layout();
    status_reaches_the_reader(h);
    concurrent_reader_never_sees_a_torn_frame(fr_init(mem));
    free(mem);
    puts("test_framering: ok");
    return 0;
}
