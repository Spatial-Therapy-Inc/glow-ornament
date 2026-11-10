#include "orn.h"

#include <stdlib.h>
#include <string.h>

static void wake(orn_t *o) {
    o->asleep = false;
    o->fading = false;
    o->fade_ms = 0;
    o->idle_ms = 0;
}

static void start_sleep(orn_t *o) {
    if (o->asleep || o->fading) return;
    o->fading = true;
    o->fade_ms = 0;
}

void orn_init(orn_t *o, int shape) {
    memset(o, 0, sizeof(*o));
    o->shape = shape >= 0 && shape < ORN_COUNT ? shape : 0;
    o->poke_at = ORN_NONE;
    o->shake_at = ORN_NONE;
}

void orn_input(orn_t *o, orn_input_t in) {
    if (o->ui_open) return;  // the system's screen takes the input itself
    const bool dark = o->asleep || o->fading;
    if (in == ORN_IN_SIDE) {
        if (dark) wake(o);
        else start_sleep(o);
        return;
    }
    if (in == ORN_IN_FACE_DOWN) {
        start_sleep(o);
        return;
    }
    if (dark) {
        // A touch or a swipe only wakes it; a shake does not (it may just be carried).
        if (in != ORN_IN_SHAKE) wake(o);
        return;
    }
    o->idle_ms = 0;
    switch (in) {
    case ORN_IN_TAP: o->poke_at = o->t; break;
    case ORN_IN_SHAKE: o->shake_at = o->t; break;
    case ORN_IN_NEXT:
        o->shape = (o->shape + 1) % ORN_COUNT;
        o->shape_changed = true;
        break;
    case ORN_IN_PREV:
        o->shape = (o->shape + ORN_COUNT - 1) % ORN_COUNT;
        o->shape_changed = true;
        break;
    default: break;
    }
}

void orn_set_shape(orn_t *o, int shape) {
    if (shape < 0 || shape >= ORN_COUNT) return;
    wake(o);
    if (shape == o->shape) return;
    o->shape = shape;
    o->shape_changed = true;
}

void orn_tick(orn_t *o, int dt_ms, int screen_off_s, float tilt_target, bool night, bool ui_open) {
    o->night = night;
    o->ui_open = ui_open;
    if (tilt_target < -1) tilt_target = -1;
    if (tilt_target > 1) tilt_target = 1;
    o->tilt += (tilt_target - o->tilt) * ORN_TILT_SMOOTH;
    if (o->asleep) return;
    o->t += dt_ms / 1000.0f * (night ? ORN_NIGHT_TEMPO : 1.0f);
    if (ui_open) {
        o->idle_ms = 0;
        return;
    }
    if (o->fading) {
        o->fade_ms += dt_ms;
        if (o->fade_ms >= ORN_FADE_MS) {
            o->fading = false;
            o->asleep = true;
        }
        return;
    }
    o->idle_ms += dt_ms;
    if (screen_off_s > 0 && o->idle_ms >= screen_off_s * 1000) start_sleep(o);
}

int orn_level(const orn_t *o, int shared_percent, float light) {
    int full = shared_percent * 255 / 100;
    // A system screen over the app (the app ring, 「回到 Pip 说？」, the pull-down, the 应用码): the shared level,
    // asleep or not, or it is drawn on a dark panel.
    if (o->ui_open) return full;
    if (o->asleep) return 0;
    if (light < 0) light = 0;
    if (light > 1) light = 1;
    if (o->night) full = full * ORN_NIGHT_PERCENT / 100;
    int level = (int)(full * light + 0.5f);
    if (o->fading) level = level * (ORN_FADE_MS - o->fade_ms) / ORN_FADE_MS;
    return level < 0 ? 0 : level;
}

int orn_ramp(int level, int target, int dt_ms) {
    int step = 255 * dt_ms / ORN_RAMP_MS;
    if (step < 1) step = 1;
    if (level < target) return level + step > target ? target : level + step;
    return level - step < target ? target : level - step;
}

float orn_since(const orn_t *o, float at) { return at <= ORN_NONE ? 1e6f : o->t - at; }

bool orn_is_night(int hour) { return hour >= ORN_NIGHT_FROM_H || hour < ORN_NIGHT_TO_H; }

void orn_shift(int64_t minutes, int *dx, int *dy) {
    static const int8_t path[8][2] = {{0, 0}, {3, 0}, {3, 3}, {0, 3}, {-3, 3}, {-3, 0}, {-3, -3}, {0, -3}};
    const int i = (int)((minutes / 2) % 8);
    *dx = path[i][0];
    *dy = path[i][1];
}

void orn_touch_down(orn_touch_t *t, int x, int y, int64_t now_ms) {
    memset(t, 0, sizeof(*t));
    if (y < ORN_TOP_EDGE_PX) return;
    t->down = true;
    t->x = x;
    t->y = y;
    t->at_ms = now_ms;
}

void orn_touch_move(orn_touch_t *t, int x, int y) {
    if (t->down && (abs(x - t->x) > ORN_SLOP_PX || abs(y - t->y) > ORN_SLOP_PX)) t->moved = true;
}

bool orn_touch_poll(orn_touch_t *t, int64_t now_ms) {
    if (!t->down || t->moved || t->long_done || now_ms - t->at_ms < ORN_LONG_MS) return false;
    t->long_done = true;
    return true;
}

orn_gesture_t orn_touch_up(orn_touch_t *t, int x, int y) {
    if (!t->down) return ORN_GESTURE_NONE;
    t->down = false;
    if (t->long_done) return ORN_GESTURE_NONE;
    const int dx = x - t->x, ax = abs(dx), ay = abs(y - t->y);
    if (ax >= ORN_SWIPE_PX && ax > ay) return dx < 0 ? ORN_GESTURE_LEFT : ORN_GESTURE_RIGHT;
    return t->moved || ax > ORN_SLOP_PX || ay > ORN_SLOP_PX ? ORN_GESTURE_NONE : ORN_GESTURE_TAP;
}

void orn_touch_cancel(orn_touch_t *t) { t->down = false; }
