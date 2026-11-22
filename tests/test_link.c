#include "../src/link.h"

#include <assert.h>
#include <stdio.h>

static void brings_up_at_once_and_stays_up(void)
{
    struct link l = {0};
    assert(link_step(&l, LINK_TICK, 0) == LINK_BRING_UP);
    assert(link_step(&l, LINK_UP_OK, 0) == LINK_NONE && l.state == LINK_UP);
    assert(link_step(&l, LINK_TICK, 5000) == LINK_NONE);
}

static void failed_bring_up_tears_down_and_retries_after_a_second(void)
{
    struct link l = {0};
    link_step(&l, LINK_TICK, 0);
    assert(link_step(&l, LINK_UP_FAILED, 100) == LINK_TEAR_DOWN && l.state == LINK_BACKOFF);
    assert(link_step(&l, LINK_TICK, 1099) == LINK_NONE);
    assert(link_step(&l, LINK_TICK, 1100) == LINK_BRING_UP);
    assert(link_step(&l, LINK_UP_FAILED, 1100) == LINK_TEAR_DOWN && l.failures == 2);
    link_step(&l, LINK_TICK, 2100);
    link_step(&l, LINK_UP_OK, 2100);
    assert(l.state == LINK_UP && l.failures == 0);
}

static void unplug_while_up_tears_down_and_retries(void)
{
    struct link l = {0};
    link_step(&l, LINK_TICK, 0);
    link_step(&l, LINK_UP_OK, 0);
    assert(link_step(&l, LINK_LOST, 300) == LINK_TEAR_DOWN && l.state == LINK_BACKOFF);
    assert(link_step(&l, LINK_TICK, 800) == LINK_NONE);
    assert(link_step(&l, LINK_TICK, 1300) == LINK_BRING_UP);
    assert(link_step(&l, LINK_UP_OK, 1300) == LINK_NONE && l.state == LINK_UP);
}

static void stray_events_are_ignored(void)
{
    struct link l = {0};
    assert(link_step(&l, LINK_LOST, 0) == LINK_NONE && l.state == LINK_DOWN);
    link_step(&l, LINK_TICK, 0);
    link_step(&l, LINK_UP_OK, 0);
    link_step(&l, LINK_LOST, 10);
    assert(link_step(&l, LINK_LOST, 20) == LINK_NONE && l.retry_at == 10 + LINK_RETRY_MS);
    assert(link_step(&l, LINK_UP_OK, 30) == LINK_NONE && l.state == LINK_BACKOFF);
}

int main(void)
{
    brings_up_at_once_and_stays_up();
    failed_bring_up_tears_down_and_retries_after_a_second();
    unplug_while_up_tears_down_and_retries();
    stray_events_are_ignored();
    puts("test_link: ok");
    return 0;
}
