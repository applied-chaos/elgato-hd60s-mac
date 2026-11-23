#ifndef HD60S_LINK_H
#define HD60S_LINK_H

#include <stdint.h>

/* Recovery for a connection the owner brings up synchronously: the dext's USB session, the camera's dext client. */
enum link_state { LINK_DOWN, LINK_CONNECTING, LINK_UP, LINK_BACKOFF };
enum link_event { LINK_TICK, LINK_UP_OK, LINK_UP_FAILED, LINK_LOST };
enum link_action { LINK_NONE, LINK_BRING_UP, LINK_TEAR_DOWN };

#define LINK_RETRY_MS 1000

struct link {
    enum link_state state;
    long retry_at;
    unsigned long failures; /* bring-up failures and losses since the last success */
};

enum link_action link_step(struct link *l, enum link_event e, long now_ms);

/* Source link flags (FUN_125f7): HDCP encrypted, and the signalling on a digital input: DVI or HDMI (neither on analogue). */
struct hd60s_link {
    int hdcp, dvi, hdmi;
};

/* digital: MST3367 b0:0x51 bit7. r01, r34: bank 1 regs 0x01 and 0x34. DVI and HDMI are only meaningful on a digital input. */
struct hd60s_link hd60s_link_from_regs(int digital, uint8_t r01, uint8_t r34);

struct hd60s;
struct hd60s_link hd60s_read_link(struct hd60s *d);

#endif
