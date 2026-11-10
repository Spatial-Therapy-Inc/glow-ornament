// Host tests for 摆件's rules (orn.c) and pictures (orn_draw.c): no device, no ESP-IDF.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/orn.h"
#include "../main/orn_draw.h"

static int s_checks, s_failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        s_checks++;                                                     \
        if (!(cond)) {                                                  \
            s_failures++;                                               \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                               \
    } while (0)

static void run(orn_t *o, int ms, int screen_off_s) {
    for (int i = 0; i < ms / 33; i++) orn_tick(o, 33, screen_off_s, 0, false, false);
}

static void test_shapes(void) {
    orn_t o;
    orn_init(&o, 12);
    CHECK(o.shape == 0);  // a stored shape out of range starts at the first
    orn_init(&o, ORN_COUNT - 1);
    orn_input(&o, ORN_IN_NEXT);
    CHECK(o.shape == 0 && o.shape_changed);
    o.shape_changed = false;
    orn_input(&o, ORN_IN_PREV);
    CHECK(o.shape == ORN_COUNT - 1 && o.shape_changed);
    o.shape_changed = false;
    orn_input(&o, ORN_IN_SIDE);
    orn_set_shape(&o, 3);  // a command: awake, on the ornament asked for
    CHECK(o.shape == 3 && o.shape_changed && !o.fading && o.idle_ms == 0);
    orn_set_shape(&o, ORN_COUNT);
    CHECK(o.shape == 3);
}

static void test_touch_and_shake(void) {
    orn_t o;
    orn_init(&o, 0);
    CHECK(orn_since(&o, o.poke_at) > 1000);  // none yet
    run(&o, 2000, 0);
    orn_input(&o, ORN_IN_TAP);
    run(&o, 1000, 0);
    CHECK(fabsf(orn_since(&o, o.poke_at) - 0.99f) < 0.05f);
    orn_input(&o, ORN_IN_SHAKE);
    CHECK(orn_since(&o, o.shake_at) < 0.01f);
}

static void test_sleep_by_screen_off(void) {
    orn_t o;
    orn_init(&o, 0);
    run(&o, 60000, 0);  // never off: stays lit
    CHECK(!o.asleep && !o.fading);
    orn_init(&o, 0);
    run(&o, 29000, 30);
    CHECK(!o.fading);
    run(&o, 1200, 30);
    CHECK(o.fading && !o.asleep);
    const int mid = orn_level(&o, 100, 1);
    CHECK(mid > 0 && mid < 255);
    run(&o, 3100, 30);
    CHECK(o.asleep);
    CHECK(orn_level(&o, 100, 1) == 0);
    // Asleep the clock stops; a touch only wakes it.
    const float t = o.t;
    run(&o, 5000, 30);
    CHECK(o.t == t);
    orn_input(&o, ORN_IN_TAP);
    CHECK(!o.asleep && o.poke_at <= ORN_NONE);
    CHECK(o.idle_ms == 0);
}

static void test_touch_keeps_it_awake(void) {
    orn_t o;
    orn_init(&o, 0);
    for (int i = 0; i < 5; i++) {
        run(&o, 20000, 30);
        orn_input(&o, ORN_IN_TAP);
    }
    CHECK(!o.asleep && !o.fading);
}

static void test_side_and_face_down(void) {
    orn_t o;
    orn_init(&o, 0);
    orn_input(&o, ORN_IN_SIDE);
    CHECK(o.fading);
    orn_input(&o, ORN_IN_SIDE);  // pressed again while fading: awake again
    CHECK(!o.fading && !o.asleep);
    orn_input(&o, ORN_IN_FACE_DOWN);
    run(&o, 3100, 0);
    CHECK(o.asleep);
    orn_input(&o, ORN_IN_SHAKE);  // carried around asleep: it stays asleep
    CHECK(o.asleep);
    orn_input(&o, ORN_IN_NEXT);  // a swipe wakes it, without moving on
    CHECK(!o.asleep && o.shape == 0);
}

static void test_system_screen(void) {
    orn_t o;
    orn_init(&o, 0);
    orn_tick(&o, 33, 30, 0, true, true);
    CHECK(orn_level(&o, 80, 0.2f) == 80 * 255 / 100);  // the system's own level, not the night's nor the scene's
    orn_input(&o, ORN_IN_NEXT);
    CHECK(o.shape == 0);  // the system's screen takes the input
    for (int i = 0; i < 2000; i++) orn_tick(&o, 33, 30, 0, false, true);
    CHECK(!o.fading);  // no going to sleep under the app ring
}

static void test_night_and_level(void) {
    CHECK(orn_is_night(22) && orn_is_night(3) && !orn_is_night(7) && !orn_is_night(15));
    orn_t o;
    orn_init(&o, 0);
    orn_tick(&o, 1000, 0, 0, true, false);
    CHECK(fabsf(o.t - ORN_NIGHT_TEMPO) < 0.001f);
    CHECK(orn_level(&o, 100, 1) == 255 * ORN_NIGHT_PERCENT / 100);
    orn_tick(&o, 1000, 0, 0, false, false);
    CHECK(orn_level(&o, 100, 0.5f) == 128);
    CHECK(orn_level(&o, 100, 3) == 255 && orn_level(&o, 100, -1) == 0);
    CHECK(orn_ramp(0, 255, 400) == 255);
    CHECK(orn_ramp(0, 255, 33) == 255 * 33 / ORN_RAMP_MS);
    CHECK(orn_ramp(200, 100, 2000) == 100);
}

static void test_tilt(void) {
    orn_t o;
    orn_init(&o, 0);
    for (int i = 0; i < 200; i++) orn_tick(&o, 33, 0, 3, false, false);
    CHECK(o.tilt > 0.99f && o.tilt <= 1);  // clamped, and reached slowly
    orn_init(&o, 0);
    orn_tick(&o, 33, 0, -1, false, false);
    CHECK(o.tilt < 0 && o.tilt > -0.1f);
}

static void test_shift(void) {
    int maxd = 0, steps = 0, lx = 0, ly = 0;
    for (int m = 0; m < 32; m++) {
        int dx, dy;
        orn_shift(m, &dx, &dy);
        if (abs(dx) > maxd) maxd = abs(dx);
        if (abs(dy) > maxd) maxd = abs(dy);
        if (dx != lx || dy != ly) steps++;
        lx = dx, ly = dy;
    }
    CHECK(maxd == 3);
    CHECK(steps >= 14);  // it keeps moving, a step every two minutes
}

static void test_gestures(void) {
    orn_touch_t t;
    orn_touch_down(&t, 200, 200, 0);
    CHECK(orn_touch_up(&t, 203, 202) == ORN_GESTURE_TAP);
    orn_touch_down(&t, 300, 200, 0);
    orn_touch_move(&t, 240, 205);
    CHECK(orn_touch_up(&t, 200, 210) == ORN_GESTURE_LEFT);
    orn_touch_down(&t, 100, 200, 0);
    orn_touch_move(&t, 160, 205);
    CHECK(orn_touch_up(&t, 200, 210) == ORN_GESTURE_RIGHT);
    orn_touch_down(&t, 200, 100, 0);
    orn_touch_move(&t, 205, 200);
    CHECK(orn_touch_up(&t, 205, 220) == ORN_GESTURE_NONE);  // a drag down is nothing
    orn_touch_down(&t, 200, 10, 0);  // the top edge is the system's pull-down
    CHECK(orn_touch_up(&t, 200, 12) == ORN_GESTURE_NONE);
    orn_touch_down(&t, 200, 200, 0);
    CHECK(!orn_touch_poll(&t, 400));
    CHECK(orn_touch_poll(&t, 600));
    CHECK(orn_touch_up(&t, 200, 200) == ORN_GESTURE_NONE);  // a long press is not a tap
}

static int red(uint16_t c) { return c >> 11; }

static void test_glow_and_firefly(void) {
    static uint16_t px[64 * 64];
    od_glow(px, 64, 2, 0xFF8040, 1);
    CHECK(red(px[32 * 64 + 32]) >= 30);
    CHECK(px[0] == 0 && red(px[32 * 64 + 0]) <= 1);
    CHECK(red(px[32 * 64 + 40]) > red(px[32 * 64 + 50]));
    od_glow(px, 64, 2, 0xFF8040, 0.5f);
    CHECK(red(px[32 * 64 + 32]) >= 14 && red(px[32 * 64 + 32]) <= 17);  // half as strong
    od_firefly(px, 64, 0xF4DE6E);
    CHECK(px[32 * 64 + 32] == px[32 * 64 + 35] || abs(red(px[32 * 64 + 32]) - red(px[32 * 64 + 35])) <= 1);  // a solid core
    CHECK(red(px[32 * 64 + 50]) < 12 && red(px[32 * 64 + 63]) <= 1);
}

static void test_moon(void) {
    // 2026-11-24: about full; 2026-11-09: about new.
    const float full = od_moon_phase(1795478400), new_ = od_moon_phase(1794182400);
    CHECK(fabsf(full - 0.5f) < 0.06f);
    CHECK(new_ < 0.06f || new_ > 0.94f);
    CHECK(od_moon_shown(0.02f) == 0.1f && od_moon_shown(0.97f) == 0.9f && od_moon_shown(0.3f) == 0.3f);
    static uint16_t m[100 * 100];
    od_moon(m, NULL, 100, 0.5f);
    CHECK(m[50 * 100 + 50] > 0xC000);  // full: bright in the middle
    CHECK(m[0] == 0);                  // and black outside the disc
    od_moon(m, NULL, 100, 0.25f);
    const uint16_t right = m[50 * 100 + 80], left = m[50 * 100 + 20];
    CHECK((right >> 11) > 20 && (left >> 11) < 8);  // first quarter: the right half lit
    od_moon(m, NULL, 100, 0.75f);
    CHECK((m[50 * 100 + 20] >> 11) > 20 && (m[50 * 100 + 80] >> 11) < 8);
    // Over a glow: the dark side is opaque (it hides the glow), the rim soft, outside clear.
    static uint8_t cover[100 * 100];
    od_moon(m, cover, 100, 0.75f);
    CHECK(cover[50 * 100 + 80] == 255 && (m[50 * 100 + 80] >> 11) < 8);
    CHECK(cover[0] == 0 && cover[50 * 100 + 50] == 255);
    CHECK(cover[50 * 100 + 99] < 255);
}

static void test_shapes_pictures(void) {
    static uint32_t px[84 * 76];
    od_triangle(px, 84, 76, 0xEA8466);
    CHECK(px[70 * 84 + 42] >> 24 == 255);  // inside, near the base
    CHECK(px[2 * 84 + 2] >> 24 == 0);       // a top corner: outside
    CHECK((px[70 * 84 + 42] & 0xFFFFFF) == 0xEA8466);
    static uint32_t semi[100 * 50];
    od_half_disc(semi, 50, 0xF0B45E);
    CHECK(semi[45 * 100 + 50] >> 24 == 255 && semi[1 * 100 + 2] >> 24 == 0);
    static uint32_t ball[56 * 56];
    od_sphere(ball, 56, 0xFFFFFF, 0x808080, 0x202020);
    CHECK(ball[28 * 56 + 28] >> 24 == 255 && ball[0] >> 24 == 0);
    CHECK((ball[18 * 56 + 18] & 0xFF) > (ball[40 * 56 + 40] & 0xFF));  // lit from the upper left
}

static void test_transform(void) {
    // A 20 x 10 white bar on black.
    static uint16_t src[20 * 10], dst[60 * 60];
    for (int i = 0; i < 20 * 10; i++) src[i] = 0xFFFF;
    // As it is, its middle (10, 5) at (30, 30): covers x 20..39, y 25..34.
    od_transform(dst, 60, 60, src, 20, 10, 10, 5, 1, 1, 0, 30, 30);
    CHECK(dst[30 * 60 + 30] == 0xFFFF && dst[30 * 60 + 21] == 0xFFFF && dst[30 * 60 + 18] == 0);
    CHECK(dst[24 * 60 + 30] == 0 && dst[33 * 60 + 30] == 0xFFFF);
    // Turned 90 degrees clockwise: now tall, x 25..34, y 20..39.
    od_transform(dst, 60, 60, src, 20, 10, 10, 5, 1, 1, 90, 30, 30);
    CHECK(dst[21 * 60 + 30] == 0xFFFF && dst[30 * 60 + 21] == 0 && dst[30 * 60 + 33] == 0xFFFF);
    // Twice as tall about its top middle (10, 0) at (30, 10): y 10..29.
    od_transform(dst, 60, 60, src, 20, 10, 10, 0, 1, 2, 0, 30, 10);
    CHECK(dst[28 * 60 + 30] == 0xFFFF && dst[31 * 60 + 30] == 0 && dst[9 * 60 + 30] == 0);
    // Gone when scaled to nothing; the rest cleared.
    od_transform(dst, 60, 60, src, 20, 10, 10, 5, 0, 0, 0, 30, 30);
    int on = 0;
    for (int i = 0; i < 60 * 60; i++) on += dst[i] != 0;
    CHECK(on == 0);
}

static void test_compose(void) {
    static uint16_t dst[40 * 40], bg[20 * 20], spot[10 * 10];
    for (int i = 0; i < 20 * 20; i++) bg[i] = 0x0841;      // a dim grey background, 20 x 20, put at (10, 10)
    for (int i = 0; i < 10 * 10; i++) spot[i] = 0x8410;    // a mid grey light
    const int all[4] = {0, 0, 39, 39};
    od_restore(dst, 40, 40, bg, 20, 20, 10, 10, all);
    CHECK(dst[0] == 0 && dst[15 * 40 + 15] == 0x0841 && dst[30 * 40 + 30] == 0);
    int box[4];
    od_add(dst, 40, 40, spot, 10, 10, 12, 12, 256, OD_ADD, box);
    CHECK(box[0] == 12 && box[1] == 12 && box[2] == 21 && box[3] == 21);
    CHECK(dst[15 * 40 + 15] == 0x8C51);  // 0x0841 + 0x8410, channel by channel
    od_add(dst, 40, 40, spot, 10, 10, 12, 12, 256, OD_ADD, NULL);
    od_add(dst, 40, 40, spot, 10, 10, 12, 12, 256, OD_ADD, NULL);
    CHECK(dst[15 * 40 + 15] == 0xFFFF);  // held at white, no carry into the next channel
    od_add(dst, 40, 40, spot, 10, 10, 35, -5, 128, OD_ADD, box);  // clipped at the right and the top
    CHECK(box[0] == 35 && box[1] == 0 && box[2] == 39 && box[3] == 4);
    CHECK(dst[2 * 40 + 36] == 0x4208);   // half of 0x8410 on black
    od_add(dst, 40, 40, spot, 10, 10, 50, 50, 256, OD_ADD, box);  // off the picture: nothing
    CHECK(box[0] > box[2]);
    const int spot_box[4] = {12, 12, 21, 21};
    od_restore(dst, 40, 40, bg, 20, 20, 10, 10, spot_box);
    CHECK(dst[15 * 40 + 15] == 0x0841);
    const int corner[4] = {30, 0, 39, 9};
    od_restore(dst, 40, 40, NULL, 0, 0, 0, 0, corner);
    CHECK(dst[2 * 40 + 36] == 0);
    // Screened: two mid greys make a lighter grey, not white; on black it is the light itself.
    static uint16_t one[1], px1[1];
    one[0] = 0x8410;
    px1[0] = 0x8410;
    od_add(px1, 1, 1, one, 1, 1, 0, 0, 256, OD_SCREEN, NULL);
    CHECK(px1[0] > 0x8410 && px1[0] < 0xFFFF && (px1[0] >> 11) == 24);  // 16 + 16 - 16 x 16 / 31 = 24 (rounded)
    px1[0] = 0;
    od_add(px1, 1, 1, one, 1, 1, 0, 0, 256, OD_SCREEN, NULL);
    CHECK(px1[0] == 0x8410);
    // The table (full strength) and the sums (less) agree: 255 / 256 of the same is within a step of it.
    px1[0] = 0x8410;
    od_add(px1, 1, 1, one, 1, 1, 0, 0, 255, OD_SCREEN, NULL);
    CHECK((px1[0] >> 11) == 24 && ((px1[0] >> 5) & 0x3F) >= 47 && ((px1[0] >> 5) & 0x3F) <= 48);
    // A turned picture added: its box covers what it drew.
    od_transform_add(dst, 40, 40, spot, 10, 10, 5, 5, 1, 1, 45, 20, 20, 256, OD_ADD, box);
    CHECK(box[0] <= 13 && box[2] >= 26 && dst[20 * 40 + 20] != 0x0841);
}

static void test_ring(void) {
    static uint16_t dst[100 * 100], bg[100 * 100];
    for (int i = 0; i < 100 * 100; i++) bg[i] = 0x0841;
    const int all[4] = {0, 0, 99, 99};
    od_restore(dst, 100, 100, bg, 100, 100, 0, 0, all);
    int box[4];
    od_ring(dst, 100, 100, 50, 50, 30, 3, 0xFFFFFF, 1, box);
    CHECK(dst[50 * 100 + 80] == 0xFFFF && dst[20 * 100 + 50] == 0xFFFF);  // on the ring, right and top
    CHECK(dst[50 * 100 + 50] == 0x0841 && dst[50 * 100 + 90] == 0x0841);  // inside and outside: untouched
    CHECK(box[0] >= 17 && box[0] <= 19 && box[2] >= 80 && box[2] <= 82 && box[1] >= 17 && box[3] <= 82);
    const uint16_t half = dst[50 * 100 + 82];                              // the soft outer edge
    CHECK(half != 0xFFFF);
    od_ring(dst, 100, 100, 50, 50, 10, 2, 0xFF0000, 0.5f, NULL);          // half red over the grey
    CHECK((dst[50 * 100 + 60] >> 11) >= 15 && (dst[50 * 100 + 60] >> 11) <= 17);
    // Putting back the same rings leaves the background everywhere.
    od_ring_restore(dst, 100, 100, bg, 100, 100, 0, 0, 50, 50, 30, 3);
    od_ring_restore(dst, 100, 100, bg, 100, 100, 0, 0, 50, 50, 10, 2);
    int off = 0;
    for (int i = 0; i < 100 * 100; i++) off += dst[i] != 0x0841;
    CHECK(off == 0);
    // Clipped at the picture's edge without trouble.
    od_ring(dst, 100, 100, 95, 5, 40, 4, 0xFFFFFF, 1, box);
    CHECK(box[2] == 99 && box[1] == 0);
    od_ring_restore(dst, 100, 100, bg, 100, 100, 0, 0, 95, 5, 40, 4);
    off = 0;
    for (int i = 0; i < 100 * 100; i++) off += dst[i] != 0x0841;
    CHECK(off == 0);
}

static void test_blob_and_aurora(void) {
    static uint16_t b[64 * 64];
    od_blob(b, 64, 0xFFE2AE, 0xFFA35C, 0xE4582F);
    CHECK(b[27 * 64 + 32] > b[32 * 64 + 60]);
    CHECK(b[0] == 0 && b[32 * 64 + 63] == 0);
    static uint16_t a[233 * 233];
    const float opa[3] = {1, 1, 1};
    od_aurora(a, 233, 233, 0.7f, opa, NULL);
    int lit = 0;
    for (int i = 0; i < 233 * 233; i++) lit += a[i] != 0;
    CHECK(lit > 233 * 233 / 5 && lit < 233 * 233);  // bands, not the whole sky
    CHECK(a[232 * 233 + 116] == 0);                  // nothing at the very bottom
    const float none[3] = {0, 0, 0};
    od_aurora(a, 233, 233, 0.7f, none, NULL);
    lit = 0;
    for (int i = 0; i < 233 * 233; i++) lit += a[i] != 0;
    CHECK(lit == 0);
    // Full width, row by row: the same picture drawn again, then only the rows that changed are cleared.
    static uint16_t full[466 * 466];
    int rows[2] = {1, 0};
    od_aurora(full, 466, 466, 0.7f, opa, rows);
    CHECK(rows[0] > 20 && rows[0] < 120 && rows[1] > 300 && rows[1] < 440);
    int on = 0;
    for (int i = 0; i < 466 * 466; i++) on += full[i] != 0;
    CHECK(on > 466 * 466 / 5);
    CHECK(full[465 * 466 + 233] == 0 && full[5 * 466 + 233] == 0);
    full[5 * 466 + 10] = 0xFFFF;  // a row not drawn now, but said to be drawn last time: cleared
    rows[0] = 0;
    od_aurora(full, 466, 466, 0.7f, opa, rows);
    CHECK(full[5 * 466 + 10] == 0);
}

int main(void) {
    test_shapes();
    test_touch_and_shake();
    test_sleep_by_screen_off();
    test_touch_keeps_it_awake();
    test_side_and_face_down();
    test_system_screen();
    test_night_and_level();
    test_tilt();
    test_shift();
    test_gestures();
    test_glow_and_firefly();
    test_moon();
    test_shapes_pictures();
    test_blob_and_aurora();
    test_transform();
    test_compose();
    test_ring();
    printf("%d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
