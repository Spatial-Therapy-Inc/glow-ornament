// 摆件's rules, without ESP-IDF or LVGL, so the host tests can run them: which ornament shows, going to sleep and
// waking, the night, the screen's level, the slow shift against burn-in, and the touch gestures.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ORN_COUNT 10
#define ORN_FADE_MS 3000        // going to sleep: the light fades out over this
#define ORN_RAMP_MS 400         // the panel's level moves at most 255 per this
#define ORN_NIGHT_FROM_H 22     // local hours that count as night: [22, 7)
#define ORN_NIGHT_TO_H 7
#define ORN_NIGHT_PERCENT 55    // the light at night, of the day's
#define ORN_NIGHT_TEMPO 0.75f   // and the animations run this much slower
#define ORN_TILT_SMOOTH 0.06f   // per tick, how far the tilt moves towards the sensor's
#define ORN_NONE (-99.0f)       // poke_at / shake_at before the first one

#define ORN_LONG_MS 500         // a press this long on the screen is a long press
#define ORN_SLOP_PX 12          // a finger moving more than this is not a tap
#define ORN_SWIPE_PX 48         // moving this far sideways is a swipe
#define ORN_TOP_EDGE_PX 24      // a press starting up here is the system's pull-down, never the app's

typedef enum {
    ORN_IN_TAP,        // a touch on the screen: the ornament answers
    ORN_IN_NEXT,       // a swipe to the left
    ORN_IN_PREV,       // a swipe to the right
    ORN_IN_SHAKE,
    ORN_IN_FACE_DOWN,  // laid face down: to sleep
    ORN_IN_SIDE,       // a short press of the side key: to sleep, or awake
} orn_input_t;

typedef struct {
    int shape;          // 0 .. ORN_COUNT - 1
    bool asleep;        // dark, the animation stopped
    bool fading;        // on the way to sleep
    int fade_ms;
    int idle_ms;        // since the last input, while awake
    float t;            // the scene's clock, seconds; slower at night, stopped while asleep
    float poke_at;      // the scene clock at the last touch, ORN_NONE before
    float shake_at;
    float tilt;         // -1 (left side down) .. 1, smoothed
    bool night;
    bool ui_open;       // a system screen (app ring, 「回到 Pip 说？」) over the app
    bool shape_changed; // to be saved
} orn_t;

void orn_init(orn_t *o, int shape);
void orn_input(orn_t *o, orn_input_t in);
/** Straight to ornament `shape` (a command, for testing): awake, as after a swipe. */
void orn_set_shape(orn_t *o, int shape);
/** One tick. screen_off_s: the shared screen-off time (0 never). tilt_target: -1..1 from the sensor. */
void orn_tick(orn_t *o, int dt_ms, int screen_off_s, float tilt_target, bool night, bool ui_open);
/** The panel's level, 0..255, from the shared brightness (percent) and the scene's light (0..1, its breathing). */
int orn_level(const orn_t *o, int shared_percent, float light);
int orn_ramp(int level, int target, int dt_ms);
/** Seconds since the last touch / shake by the scene clock; a large number when there was none. */
float orn_since(const orn_t *o, float at);
bool orn_is_night(int hour);
/** Against burn-in: the whole picture walks a small square, one step every two minutes, at most 3 px. */
void orn_shift(int64_t minutes, int *dx, int *dy);

typedef enum { ORN_GESTURE_NONE, ORN_GESTURE_TAP, ORN_GESTURE_LEFT, ORN_GESTURE_RIGHT } orn_gesture_t;

typedef struct {
    bool down, moved, long_done;
    int x, y;
    int64_t at_ms;
} orn_touch_t;

void orn_touch_down(orn_touch_t *t, int x, int y, int64_t now_ms);
void orn_touch_move(orn_touch_t *t, int x, int y);
/** True once when a press has been held ORN_LONG_MS without moving: long presses are not used, but are not taps. */
bool orn_touch_poll(orn_touch_t *t, int64_t now_ms);
orn_gesture_t orn_touch_up(orn_touch_t *t, int x, int y);
void orn_touch_cancel(orn_touch_t *t);
