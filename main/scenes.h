// The ten ornaments, drawn with LVGL: each is built into an empty root object and then moved one frame at a time.
// Their look and timing follow the design (摆件 设计稿); the numbers are its, in the 466 x 466 screen's pixels.
#pragma once

#include <stdbool.h>

#include "lvgl.h"

typedef struct {
    float t;      // the scene's clock, seconds
    float dt;     // since the last frame, seconds (slower at night, like t)
    float tilt;   // -1 .. 1: which side is down
    float poke;   // seconds since the last touch (very large: none yet)
    float shake;  // seconds since the last shake
} orn_frame_t;

/** The pictures every scene shares (glows, blobs, spheres, shapes), made once in PSRAM. False when there is no room. */
bool orn_scenes_init(void);
/** Builds ornament i (0 .. ORN_COUNT - 1) in root, emptied first. */
void orn_scene_build(int i, lv_obj_t *root);
/** One frame of the ornament built last; returns its light, 0..1, which the panel's level follows (breathing,
 *  flicker). */
float orn_scene_frame(const orn_frame_t *f);
/** A short name for logs and commands: "mushroom" ... "halo". */
const char *orn_scene_key(int i);
