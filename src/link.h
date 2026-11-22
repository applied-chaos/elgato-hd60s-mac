#ifndef HD60S_LINK_H
#define HD60S_LINK_H

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

#endif
