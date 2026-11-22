#include "../src/deinterlace.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define W 4
#define H 6
#define ROW (W * 2)

static void make(uint8_t *f, int shift)
{
    /* Odd rows (field 1) are `shift` ahead of even rows (field 0). */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < ROW; x++)
            f[y * ROW + x] = (uint8_t)(x / 4 * 40 + 20 + (y & 1 ? shift : 0));
}

static void test_still_is_woven(void)
{
    struct deint d = {0};
    uint8_t a[H * ROW], b[H * ROW], want[H * ROW];
    for (int i = 0; i < H * ROW; i++)
        a[i] = want[i] = (uint8_t)(i * 7);
    memcpy(b, a, sizeof a);
    deint_frame(&d, a, W, H);
    deint_frame(&d, b, W, H);
    assert(memcmp(b, want, sizeof b) == 0);
    deint_free(&d);
}

static void test_motion_interpolates_second_field(void)
{
    struct deint d = {0};
    uint8_t a[H * ROW], b[H * ROW];
    make(a, 0);
    deint_frame(&d, a, W, H);
    make(b, 100);
    deint_frame(&d, b, W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < ROW; x++) {
            uint8_t even = (uint8_t)(x / 4 * 40 + 20);
            assert(b[y * ROW + x] == even);
        }
    deint_free(&d);
}

static void test_motion_only_where_it_moved(void)
{
    struct deint d = {0};
    uint8_t a[H * ROW], b[H * ROW], want[H * ROW];
    make(a, 0);
    deint_frame(&d, a, W, H);
    make(b, 0);
    b[3 * ROW + 5] = 200;
    memcpy(want, b, sizeof b);
    for (int x = 4; x < 8; x++)
        want[3 * ROW + x] = (uint8_t)((b[2 * ROW + x] + b[4 * ROW + x] + 1) / 2);
    deint_frame(&d, b, W, H);
    assert(memcmp(b, want, sizeof b) == 0);
    deint_free(&d);
}

static void test_size_change_resets(void)
{
    struct deint d = {0};
    uint8_t a[H * ROW], b[2 * ROW];
    make(a, 0);
    deint_frame(&d, a, W, H);
    memset(b, 9, sizeof b);
    deint_frame(&d, b, W, 2);
    assert(b[ROW] == 9);
    deint_free(&d);
}

int main(void)
{
    test_still_is_woven();
    test_motion_interpolates_second_field();
    test_motion_only_where_it_moved();
    test_size_change_resets();
    puts("deinterlace tests passed");
    return 0;
}
