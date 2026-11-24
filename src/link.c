#include "link.h"

#include "device.h"

static enum link_action back_off(struct link *l, long now_ms)
{
    l->state = LINK_BACKOFF;
    l->retry_at = now_ms + LINK_RETRY_MS;
    l->failures++;
    return LINK_TEAR_DOWN;
}

enum link_action link_step(struct link *l, enum link_event e, long now_ms)
{
    switch (l->state) {
    case LINK_DOWN:
    case LINK_BACKOFF:
        if (e != LINK_TICK || (l->state == LINK_BACKOFF && now_ms < l->retry_at))
            return LINK_NONE;
        l->state = LINK_CONNECTING;
        return LINK_BRING_UP;
    case LINK_CONNECTING:
        if (e == LINK_UP_OK) {
            l->state = LINK_UP;
            l->failures = 0;
            return LINK_NONE;
        }
        return e == LINK_UP_FAILED ? back_off(l, now_ms) : LINK_NONE;
    case LINK_UP:
        return e == LINK_LOST ? back_off(l, now_ms) : LINK_NONE;
    }
    return LINK_NONE;
}

struct hd60s_link hd60s_link_from_regs(int digital, uint8_t r01, uint8_t r34)
{
    return (struct hd60s_link){.hdcp = r34 >> 7 | (r01 & 1), .dvi = digital && !(r01 >> 2 & 1),
                                .hdmi = digital && (r01 >> 2 & 1)};
}

struct hd60s_link hd60s_read_link(struct hd60s *d)
{
    unsigned failures = d->failures;
    int digital = mst_read(d, 0, 0x51) & 0x80;
    uint8_t r01 = (uint8_t)mst_read(d, 1, 0x01), r34 = (uint8_t)mst_read(d, 1, 0x34);
    if (d->failures == failures)
        d->link = hd60s_link_from_regs(digital != 0, r01, r34);
    return d->link;
}
