// The ten ornaments. Everything sits on black and most of it is light, so the glows and the drawn parts are added
// (LV_BLEND_MODE_ADDITIVE): a part's black adds nothing and overlapping light gets brighter, as in the design.
// Each scene's big glow is the bottom layer, on black, so it is drawn normally: the same picture, and LVGL copies
// it instead of adding it pixel by pixel (that alone was most of a frame wherever something moved over it). Only
// the parts that must hide what is behind them (the planet, the moon, the mobile's shapes, the stone) are drawn
// normally too. A scene keeps its objects in statics here; building the next one empties the root.
#include "scenes.h"

#include <math.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "orn.h"
#include "orn_draw.h"
#include "yaykit_ui.h"
#include "yk_app_summary.h"

static const char *TAG = "scenes";

#define TAU 6.2831853f
#define CX 233.0f

// The drawn parts (art/*.svg, made into RGB565 by tools/make_art.py).
extern const uint8_t mushroom_start[] asm("_binary_mushroom_rgb565_start");
extern const uint8_t flame_start[] asm("_binary_flame_rgb565_start");
extern const uint8_t bell_start[] asm("_binary_bell_rgb565_start");

static lv_obj_t *s_root;
static int s_scene;

// ---------------------------------------------------------------- pictures

static lv_image_dsc_t dsc_of(const void *data, int w, int h, lv_color_format_t cf, int bpp) {
    return (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = cf, .w = w, .h = h, .stride = w * bpp},
        .data_size = (uint32_t)(w * h * bpp),
        .data = data,
    };
}

static void *psram(size_t bytes) { return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM); }

// Pictures made the first time a scene asks for them, at the size it draws them (a picture scaled every frame costs
// LVGL far more than one drawn as it is), and kept: PSRAM has room for all of them.
typedef enum { PIC_GLOW, PIC_FLY, PIC_BLOB_A, PIC_BLOB_B } pic_kind_t;

#define PICTURES 40
static struct {
    pic_kind_t kind;
    int size;
    float power, strength;
    uint32_t rgb;
    lv_image_dsc_t dsc;
} s_pics[PICTURES];
static int s_pic_count;

/** rgb at k (0..1) of its strength. */
static uint32_t dim(uint32_t rgb, float k) {
    uint32_t out = 0;
    for (int s = 0; s <= 16; s += 8) out |= (uint32_t)(((rgb >> s) & 0xFF) * k + 0.5f) << s;
    return out;
}

static const lv_image_dsc_t *picture(pic_kind_t kind, int size, float power, uint32_t rgb, float strength) {
    for (int i = 0; i < s_pic_count; i++) {
        if (s_pics[i].kind == kind && s_pics[i].size == size && s_pics[i].power == power && s_pics[i].rgb == rgb &&
            s_pics[i].strength == strength) {
            return &s_pics[i].dsc;
        }
    }
    uint16_t *px = s_pic_count < PICTURES ? psram((size_t)size * size * 2) : NULL;
    if (!px) {
        ESP_LOGE(TAG, "no room for a picture of %d", size);
        return NULL;
    }
    switch (kind) {
    case PIC_GLOW: od_glow(px, size, power, rgb, strength); break;
    case PIC_FLY: od_firefly(px, size, rgb); break;
    // The lava's blobs at the design's 0.85 already, so they are screened at full strength (by table, the fast way).
    case PIC_BLOB_A: od_blob(px, size, dim(0xFFE2AE, 0.85f), dim(0xFFA35C, 0.85f), dim(0xE4582F, 0.85f)); break;
    case PIC_BLOB_B: od_blob(px, size, dim(0xFFF0C4, 0.85f), dim(0xFFC46B, 0.85f), dim(0xF0884A, 0.85f)); break;
    }
    s_pics[s_pic_count] = (typeof(s_pics[0])){.kind = kind, .size = size, .power = power, .rgb = rgb, .strength = strength,
                                              .dsc = dsc_of(px, size, size, LV_COLOR_FORMAT_RGB565, 2)};
    return &s_pics[s_pic_count++].dsc;
}

#define STONE 56
#define ORB 132
#define MOON 330
#define SEMI 50
#define TRI_W 84
#define TRI_H 76
#define SCREEN 466  // the full-screen pictures: the aurora's, or the canvas (one scene at a time)

static lv_image_dsc_t s_stone, s_orb, s_semi, s_tri, s_moon, s_screen;
static lv_image_dsc_t s_mushroom, s_flame, s_bell;
static uint16_t *s_moon_px, *s_buf[2];  // s_buf: the two full-screen pictures (see the canvas)
static float s_moon_phase = -1;

static bool compose_start(void);  // the canvas's helper task, below

bool orn_scenes_init(void) {
    uint32_t *stone = psram(STONE * STONE * 4), *orb = psram(ORB * ORB * 4), *semi = psram(SEMI * 2 * SEMI * 4);
    uint32_t *tri = psram(TRI_W * TRI_H * 4);
    s_moon_px = psram(MOON * MOON * 3);  // RGB565A8: the colours, then the cover
    s_buf[0] = psram(SCREEN * SCREEN * 2);
    s_buf[1] = psram(SCREEN * SCREEN * 2);
    if (!stone || !orb || !semi || !tri || !s_moon_px || !s_buf[0] || !s_buf[1]) {
        ESP_LOGE(TAG, "no room for the pictures");
        return false;
    }
    od_sphere(stone, STONE, 0xFFFBF0, 0xF3E6C8, 0xD9BE8E);
    od_sphere(orb, ORB, 0xFFF6E2, 0xF5C47E, 0xC0743A);
    od_half_disc(semi, SEMI, 0xF0B45E);
    od_triangle(tri, TRI_W, TRI_H, 0xEA8466);
    s_stone = dsc_of(stone, STONE, STONE, LV_COLOR_FORMAT_ARGB8888, 4);
    s_orb = dsc_of(orb, ORB, ORB, LV_COLOR_FORMAT_ARGB8888, 4);
    s_semi = dsc_of(semi, SEMI * 2, SEMI, LV_COLOR_FORMAT_ARGB8888, 4);
    s_tri = dsc_of(tri, TRI_W, TRI_H, LV_COLOR_FORMAT_ARGB8888, 4);
    s_moon = dsc_of(s_moon_px, MOON, MOON, LV_COLOR_FORMAT_RGB565A8, 2);
    s_moon.data_size = MOON * MOON * 3;
    s_screen = dsc_of(s_buf[0], SCREEN, SCREEN, LV_COLOR_FORMAT_RGB565, 2);
    s_mushroom = dsc_of(mushroom_start, 346, 371, LV_COLOR_FORMAT_RGB565, 2);
    s_flame = dsc_of(flame_start, 136, 236, LV_COLOR_FORMAT_RGB565, 2);
    s_bell = dsc_of(bell_start, 172, 122, LV_COLOR_FORMAT_RGB565, 2);
    return compose_start();
}

// ---------------------------------------------------------------- objects

static lv_obj_t *add_image(const lv_image_dsc_t *d, bool additive) {
    lv_obj_t *o = lv_image_create(s_root);
    lv_image_set_src(o, d);
    lv_obj_set_clickable(o, false);
    if (additive) lv_obj_set_style_blend_mode(o, LV_BLEND_MODE_ADDITIVE, 0);
    return o;
}

/** Puts an image's middle (its pivot unless moved) at (cx, cy). */
static void image_at(lv_obj_t *o, const lv_image_dsc_t *d, float cx, float cy) {
    lv_obj_set_pos(o, (int32_t)lroundf(cx - d->header.w / 2.0f), (int32_t)lroundf(cy - d->header.h / 2.0f));
}

static lv_opa_t opa_of(float k) { return (lv_opa_t)(k <= 0 ? 0 : k >= 1 ? 255 : k * 255 + 0.5f); }

/** A glow of colour `color` at `strength` (0..1) in the middle, added to what is under it. */
static lv_obj_t *add_glow(int size, float power, uint32_t color, float strength, float cx, float cy) {
    const lv_image_dsc_t *d = picture(PIC_GLOW, size, power, color, strength);
    lv_obj_t *o = add_image(d, true);
    if (d) image_at(o, d, cx, cy);
    return o;
}

/** The scene's big glow: the first thing in it, on black, so drawn normally (see the top). Never fade it frame by
 *  frame: every change redraws all of it; the panel's level does the breathing. */
static lv_obj_t *add_base_glow(int size, float power, uint32_t color, float strength, float cx, float cy) {
    const lv_image_dsc_t *d = picture(PIC_GLOW, size, power, color, strength);
    lv_obj_t *o = add_image(d, false);
    if (d) image_at(o, d, cx, cy);
    return o;
}

/** A filled dot; its radius is kept in user data for dot_at. */
static lv_obj_t *add_dot(float r, uint32_t color) {
    lv_obj_t *o = lv_obj_create(s_root);
    lv_obj_remove_style_all(o);
    const int d = (int)lroundf(2 * r);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_clickable(o, false);
    lv_obj_set_user_data(o, (void *)(intptr_t)d);
    return o;
}

static void dot_at(lv_obj_t *o, float cx, float cy, float opa) {
    const int d = (int)(intptr_t)lv_obj_get_user_data(o);
    lv_obj_set_pos(o, (int32_t)lroundf(cx - d / 2.0f), (int32_t)lroundf(cy - d / 2.0f));
    const lv_opa_t k = opa_of(opa);
    if (lv_obj_get_style_opa(o, 0) != k) lv_obj_set_style_opa(o, k, 0);
}

#define LINE_PTS 24
typedef struct {
    lv_obj_t *o;
    lv_point_precise_t p[LINE_PTS];
    int width;
} line_t;

static void line_new(line_t *l, int width, uint32_t color, float opa) {
    l->o = lv_line_create(s_root);
    l->width = width;
    lv_obj_set_style_line_width(l->o, width, 0);
    lv_obj_set_style_line_color(l->o, lv_color_hex(color), 0);
    lv_obj_set_style_line_opa(l->o, opa_of(opa), 0);
    // Round caps only on opaque lines: a translucent line's caps overlap at every joint and show as beads.
    lv_obj_set_style_line_rounded(l->o, opa >= 1, 0);
    lv_obj_set_clickable(l->o, false);
}

/** Points in screen pixels, x y x y ...: the object is moved to their box so only that box is redrawn. */
static void line_set(line_t *l, const float *xy, int n) {
    if (n > LINE_PTS) n = LINE_PTS;
    float minx = 1e9f, miny = 1e9f;
    for (int i = 0; i < n; i++) {
        if (xy[2 * i] < minx) minx = xy[2 * i];
        if (xy[2 * i + 1] < miny) miny = xy[2 * i + 1];
    }
    const int ox = (int)floorf(minx) - l->width, oy = (int)floorf(miny) - l->width;
    for (int i = 0; i < n; i++) {
        l->p[i].x = (lv_value_precise_t)(xy[2 * i] - ox);
        l->p[i].y = (lv_value_precise_t)(xy[2 * i + 1] - oy);
    }
    lv_obj_set_pos(l->o, ox, oy);
    lv_line_set_points(l->o, l->p, (uint32_t)n);
}

// ---------------------------------------------------------------- the canvas
// The fireflies, the lava's blobs, the candle's flame and the jelly's bell are big soft lights that move every frame.
// LVGL adding a picture to what is under it goes pixel by pixel through its general path (5 frames a second with the
// fireflies), so these scenes add their lights themselves (orn_draw.c) into one full-screen picture that starts as
// their glow, and LVGL shows it as it is, on top of nothing. Dots and lines stay LVGL's, over the canvas.
//
// Two such pictures: a helper task adds the next frame's lights into the one not on the screen while LVGL draws the
// one that is (the chip has two cores; LVGL's task is not tied to either), and at the next frame they change places.
// The frame then takes the longer of the two jobs instead of both. In the picture being drawn, each light's box from
// the last time is put back from the glow first; after the change, only the boxes of the lights in either picture are
// redrawn. The aurora uses the same two pictures and task. LVGL's image cache is off (CONFIG_LV_CACHE_DEF_SIZE 0)
// and a picture in memory is drawn from its pointer each time, so changing s_screen.data is enough.

#define LIGHTS 16
typedef struct {
    const lv_image_dsc_t *pic;  // a picture, or NULL
    bool turned;                // scaled and turned (od_transform_add), else as it is (od_add)
    float px, py, sx, sy, deg;  // turned: the picture's point (px, py) at (x, y), scaled sx, sy, turned deg
    float x, y, opa;            // as it is: the picture's middle at (x, y); a ring: its middle
    od_blend_t mode;            // added, or screened (the lava, as in the design)
    bool ring;                  // a ring (od_ring) instead of a picture: radius r, `width` thick, in rgb
    float r, width;
    uint32_t rgb;
} light_t;

static lv_obj_t *s_canvas_obj;   // the canvas, or the aurora
static int s_shown;              // the picture on the screen; the helper draws the other
static const lv_image_dsc_t *s_canvas_bg;
static int s_bg_x, s_bg_y;
static int s_box[2][LIGHTS][4];  // per picture: each light's box when last drawn into it
static light_t s_drawn[2][LIGHTS];  // ... and the light itself (a ring is put back by its own pixels, not its box)
static int s_rows[2][2];         // per picture: the aurora's rows when last drawn into it
static bool s_fill[2];           // per picture: the background changed, put all of it back first
static light_t s_next[LIGHTS];   // the lights of the frame being worked out
static struct {
    int target;
    bool aurora, filled;
    const lv_image_dsc_t *bg;
    light_t light[LIGHTS];
    float flow, opa[3];
} s_job;
static bool s_busy;  // a job handed over and not yet collected
static SemaphoreHandle_t s_go, s_done;

static void put_back(uint16_t *px, const lv_image_dsc_t *bg, const int box[4]) {
    od_restore(px, SCREEN, SCREEN, bg ? (const uint16_t *)bg->data : NULL, bg ? (int)bg->header.w : 0,
               bg ? (int)bg->header.h : 0, s_bg_x, s_bg_y, box);
}

static const int ALL[4] = {0, 0, SCREEN - 1, SCREEN - 1};

static void compose_task(void *arg) {
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        const int b = s_job.target;
        uint16_t *px = s_buf[b];
        s_job.filled = false;
        if (s_job.aurora) {
            od_aurora(px, SCREEN, SCREEN, s_job.flow, s_job.opa, s_rows[b]);
        } else {
            if (s_fill[b]) {
                put_back(px, s_job.bg, ALL);
                s_fill[b] = false;
                s_job.filled = true;
            } else {
                for (int i = 0; i < LIGHTS; i++) {
                    const light_t *was = &s_drawn[b][i];
                    if (was->ring) {
                        const lv_image_dsc_t *bg = s_job.bg;
                        od_ring_restore(px, SCREEN, SCREEN, bg ? (const uint16_t *)bg->data : NULL,
                                        bg ? (int)bg->header.w : 0, bg ? (int)bg->header.h : 0, s_bg_x, s_bg_y, was->x,
                                        was->y, was->r, was->width);
                    } else {
                        put_back(px, s_job.bg, s_box[b][i]);
                    }
                }
            }
            for (int i = 0; i < LIGHTS; i++) {
                const light_t *l = &s_job.light[i];
                int *box = s_box[b][i];
                box[0] = 1, box[2] = 0;
                s_drawn[b][i] = *l;
                if (l->ring) {
                    od_ring(px, SCREEN, SCREEN, l->x, l->y, l->r, l->width, l->rgb, l->opa, box);
                    continue;
                }
                if (!l->pic) continue;
                const uint16_t *src = (const uint16_t *)l->pic->data;
                const int w = l->pic->header.w, h = l->pic->header.h, opa = (int)(l->opa * 256 + 0.5f);
                if (l->turned) {
                    od_transform_add(px, SCREEN, SCREEN, src, w, h, l->px, l->py, l->sx, l->sy, l->deg, l->x, l->y, opa,
                                     l->mode, box);
                } else {
                    od_add(px, SCREEN, SCREEN, src, w, h, (int)lroundf(l->x - w / 2.0f), (int)lroundf(l->y - h / 2.0f),
                           opa, l->mode, box);
                }
            }
        }
        xSemaphoreGive(s_done);
    }
}

static bool compose_start(void) {
    s_go = xSemaphoreCreateBinary();
    s_done = xSemaphoreCreateBinary();
    // Below LVGL's own task, on whichever core is free.
    return s_go && s_done && xTaskCreate(compose_task, "orn_compose", 4096, NULL, 3, NULL) == pdPASS;
}

/** Redraws the box {x0, y0, x1, y1} (in the root's pixels) of the full-screen picture. */
static void redraw_box(const int box[4]) {
    if (box[0] > box[2] || box[1] > box[3]) return;
    lv_area_t a;
    lv_obj_get_coords(s_canvas_obj, &a);  // the root walks a few pixels against burn-in
    const lv_area_t b = {a.x1 + box[0], a.y1 + box[1], a.x1 + box[2], a.y1 + box[3]};
    lv_obj_invalidate_area(s_canvas_obj, &b);
}

/** Redraws two boxes, as one when they meet (a light moves a pixel or two a frame): LVGL keeps 32 boxes a frame and
 *  redraws the whole screen past that. */
static void redraw_both(const int *w, const int *n) {
    const bool w_on = w[0] <= w[2], n_on = n[0] <= n[2];
    if (w_on && n_on && w[0] <= n[2] + 1 && n[0] <= w[2] + 1 && w[1] <= n[3] + 1 && n[1] <= w[3] + 1) {
        const int u[4] = {w[0] < n[0] ? w[0] : n[0], w[1] < n[1] ? w[1] : n[1], w[2] > n[2] ? w[2] : n[2],
                          w[3] > n[3] ? w[3] : n[3]};
        redraw_box(u);
    } else {
        redraw_box(w);
        redraw_box(n);
    }
}

/** Four boxes over a ring's pixels (top, bottom, left, right; each a quarter of it), much less than its square, or
 *  one box for a small ring. For a light that was and is a ring, each box covers both. */
static void redraw_rings(const light_t *was, const light_t *now) {
    int out[4][4];
    bool any = false, small = true;
    for (int n = 0; n < 2; n++) {
        const light_t *l = n ? now : was;
        if (!l->ring) continue;
        const float ri = l->r - l->width / 2 - 2, ro = l->r + l->width / 2 + 2, k = 0.7072f;
        if (ro >= 48) small = false;
        const int L = (int)floorf(l->x - ro), R = (int)ceilf(l->x + ro), T = (int)floorf(l->y - ro), B = (int)ceilf(l->y + ro);
        const int kx0 = (int)floorf(l->x - ro * k), kx1 = (int)ceilf(l->x + ro * k);
        const int ky0 = (int)floorf(l->y - ro * k), ky1 = (int)ceilf(l->y + ro * k);
        const float rk = ri > 0 ? ri * k : 0;
        const int ix0 = (int)ceilf(l->x - rk), ix1 = (int)floorf(l->x + rk), iy0 = (int)ceilf(l->y - rk),
                  iy1 = (int)floorf(l->y + rk);
        const int r4[4][4] = {{kx0, T, kx1, iy0}, {kx0, iy1, kx1, B}, {L, ky0, ix0, ky1}, {ix1, ky0, R, ky1}};
        for (int q = 0; q < 4; q++) {
            if (!any) {
                memcpy(out[q], r4[q], sizeof(out[q]));
                continue;
            }
            out[q][0] = r4[q][0] < out[q][0] ? r4[q][0] : out[q][0];
            out[q][1] = r4[q][1] < out[q][1] ? r4[q][1] : out[q][1];
            out[q][2] = r4[q][2] > out[q][2] ? r4[q][2] : out[q][2];
            out[q][3] = r4[q][3] > out[q][3] ? r4[q][3] : out[q][3];
        }
        any = true;
    }
    if (!any) return;
    if (small) {
        const int box[4] = {out[2][0], out[0][1], out[3][2], out[1][3]};
        redraw_box(box);
        return;
    }
    for (int q = 0; q < 4; q++) redraw_box(out[q]);
}

/** Waits for the helper's picture, if one was asked for, puts it on the screen and redraws what changed. */
static void collect(void) {
    if (!s_busy) return;
    xSemaphoreTake(s_done, portMAX_DELAY);
    s_busy = false;
    const int f = s_shown, b = s_job.target;
    s_shown = b;
    s_screen.data = (const uint8_t *)s_buf[b];
    if (s_job.filled) {
        lv_obj_invalidate(s_canvas_obj);
    } else if (s_job.aurora) {
        const int was[4] = {0, s_rows[f][0], SCREEN - 1, s_rows[f][1]}, now[4] = {0, s_rows[b][0], SCREEN - 1, s_rows[b][1]};
        redraw_both(was, now);
    } else {
        for (int i = 0; i < LIGHTS; i++) {
            if (s_drawn[f][i].ring || s_drawn[b][i].ring) redraw_rings(&s_drawn[f][i], &s_drawn[b][i]);
            else redraw_both(s_box[f][i], s_box[b][i]);
        }
    }
}

/** Both pictures as `bg` (a glow picture, its middle at (cx, cy); NULL black) and on the screen: the first object of
 *  the scene, everything else drawn over it. */
static void canvas_build(const lv_image_dsc_t *bg, float cx, float cy) {
    s_canvas_bg = bg;
    s_bg_x = bg ? (int)lroundf(cx - bg->header.w / 2.0f) : 0;
    s_bg_y = bg ? (int)lroundf(cy - bg->header.h / 2.0f) : 0;
    put_back(s_buf[0], bg, ALL);
    memcpy(s_buf[1], s_buf[0], SCREEN * SCREEN * 2);
    for (int b = 0; b < 2; b++) {
        s_fill[b] = false;
        for (int i = 0; i < LIGHTS; i++) s_box[b][i][0] = 1, s_box[b][i][2] = 0;
        memset(s_drawn[b], 0, sizeof(s_drawn[b]));
    }
    s_job.bg = bg;
    s_shown = 0;
    s_screen.data = (const uint8_t *)s_buf[0];
    s_canvas_obj = add_image(&s_screen, false);
    lv_obj_set_pos(s_canvas_obj, 0, 0);
}

/** Another background from this frame on (the candle blown out, lit again). Keeps the glow where it was. */
static void canvas_set_bg(const lv_image_dsc_t *bg) { s_canvas_bg = bg; }

/** A frame's lights start: none. */
static void canvas_begin(void) {
    memset(s_next, 0, sizeof(s_next));
}

/** Light i: picture `pic` (a light on black) with its middle at (x, y), at opa (0..1). */
static void canvas_light(int i, const lv_image_dsc_t *pic, float x, float y, float opa, od_blend_t mode) {
    if (i >= LIGHTS) return;
    s_next[i] = (light_t){.pic = pic, .x = x, .y = y, .opa = opa, .mode = mode};
}

/** Light i, scaled and turned: the picture's point (px, py) at (x, y), scaled sx, sy about it, turned deg. */
static void canvas_light_turned(int i, const lv_image_dsc_t *pic, float px, float py, float sx, float sy, float deg,
                                float x, float y, float opa, od_blend_t mode) {
    if (i >= LIGHTS) return;
    s_next[i] = (light_t){.pic = pic, .turned = true, .px = px, .py = py, .sx = sx, .sy = sy, .deg = deg, .x = x, .y = y,
                          .opa = opa, .mode = mode};
}

/** Light i, a ring: radius r (to the middle of its line) about (x, y), `width` thick, in rgb at opa, drawn over. */
static void canvas_ring(int i, float x, float y, float r, float width, uint32_t rgb, float opa) {
    if (i >= LIGHTS) return;
    s_next[i] = (light_t){.ring = true, .x = x, .y = y, .r = r, .width = width, .rgb = rgb, .opa = opa};
}

/** The frame's lights are all there: show the last frame's picture and hand this one to the helper. */
static void canvas_end(void) {
    collect();
    if (s_canvas_bg != s_job.bg) s_fill[0] = s_fill[1] = true;
    s_job.bg = s_canvas_bg;
    s_job.aurora = false;
    s_job.target = !s_shown;
    memcpy(s_job.light, s_next, sizeof(s_next));
    s_busy = true;
    xSemaphoreGive(s_go);
}

static float wrapf(float x, float m) {
    const float r = fmodf(x, m);
    return r < 0 ? r + m : r;
}

static float minf(float a, float b) { return a < b ? a : b; }
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
/** A touch's answer fading out: 1 at the touch, falling at `rate` per second; 0 after `until` seconds. */
static float fade(float since, float rate, float until) { return since < until ? expf(-since * rate) : 0; }

// ---------------------------------------------------------------- 1 蘑菇

static const float SPORES[6][4] = {{150, 13, 0.0f, 3}, {205, 11, 0.35f, 2.4f}, {260, 14, 0.6f, 3.4f},
                                   {310, 12, 0.2f, 2.2f}, {180, 15, 0.8f, 2.8f}, {285, 10, 0.5f, 2.4f}};
static lv_obj_t *s_spore[6];

static void mushroom_build(void) {
    add_base_glow(466, 1.6f, 0xFFC27A, 0.75f, 233, 236);
    lv_obj_t *body = add_image(&s_mushroom, true);
    lv_obj_set_pos(body, 60, 95);
    for (int i = 0; i < 6; i++) s_spore[i] = add_dot(SPORES[i][3], 0xFFEFC8);
}

static float mushroom_frame(const orn_frame_t *f) {
    const float b = 0.5f - 0.5f * cosf(TAU * f->t / 6);
    const float boost = fade(f->poke, 1.3f, 3) + (f->shake < 1 ? 0.6f * sinf((float)M_PI * f->shake) : 0);
    const float speed = f->shake < 4 ? 1 + 2 * expf(-f->shake) : 1;
    for (int i = 0; i < 6; i++) {
        // Spores fall slowly from the gills and fade; a shake sends a puff of them, tilting carries them aside.
        const float k = wrapf(f->t * speed / SPORES[i][1] + SPORES[i][2], 1);
        const float x = SPORES[i][0] + (SPORES[i][0] - CX) * 0.4f * k + 12 * sinf(f->t * 0.6f + i * 1.7f) + f->tilt * 60 * k;
        dot_at(s_spore[i], x, 298 + k * 168, sinf((float)M_PI * k) * (0.7f + 0.3f * boost));
    }
    return clamp01(0.78f + 0.22f * b + 0.25f * boost);
}

// ---------------------------------------------------------------- 2 萤火虫

#define FLIES 14
static const lv_image_dsc_t *s_fly_pic[FLIES];

static float fly_depth(int i) { return 0.25f + ((i * 37) % 75) / 100.0f; }

static void fireflies_build(void) {
    canvas_build(picture(PIC_GLOW, 466, 2.0f, 0xE8D66E, 0.28f), 233, 256);
    // Each firefly at its own size (near ones bigger), drawn as it is.
    for (int i = 0; i < FLIES; i++) s_fly_pic[i] = picture(PIC_FLY, 2 * (int)lroundf(20 + 40 * fly_depth(i)), 0, 0xF4DE6E, 1);
}

static float fireflies_frame(const orn_frame_t *f) {
    // A touch draws them in towards the middle for a moment; a shake scatters them out to the edge.
    const float gather = f->poke < 6 ? 0.55f * expf(-f->poke * 0.7f) * minf(1, f->poke * 3) : 0;
    const float scatter = f->shake < 6 ? 0.9f * expf(-f->shake * 0.8f) : 0;
    canvas_begin();
    for (int i = 0; i < FLIES; i++) {
        const float home_a = i * 2.399f, home_d = 40 + (i * 53) % 150, depth = fly_depth(i);
        const float phase = i * 0.83f;
        const float a = home_a + 0.05f * f->t * (i % 2 ? 1 : -1);
        const float d = home_d * (1 - gather) * (1 + scatter);
        float x = CX + d * cosf(a) + 46 * sinf(f->t * (0.21f + 0.03f * (i % 5)) + phase) + f->tilt * 80;
        float y = CX + d * sinf(a) + 34 * cosf(f->t * (0.17f + 0.02f * (i % 4)) + phase * 1.4f) + fabsf(f->tilt) * 40;
        const float dx = x - CX, dy = y - CX, dist = sqrtf(dx * dx + dy * dy);
        if (dist > 205) {
            x = CX + dx * 205 / dist;
            y = CX + dy * 205 / dist;
        }
        const float blink = powf(fmaxf(0, sinf(f->t * (0.7f + 0.09f * i) + phase * 2)), 2);
        canvas_light(i, s_fly_pic[i], x, y, minf(1, (0.35f + 0.65f * blink) * (0.65f + 0.35f * depth) + 0.5f * gather),
                     OD_ADD);
    }
    canvas_end();
    return 1;
}

// ---------------------------------------------------------------- 3 熔岩

static const float BLOBS[6][4] = {{96, 26, 0.0f, -0.3f}, {78, 21, 1.7f, 0.35f}, {62, 17, 3.1f, 0.0f},
                                  {88, 31, 4.4f, -0.45f}, {54, 15, 5.6f, 0.5f}, {70, 23, 2.4f, 0.15f}};
static const lv_image_dsc_t *s_blob_pic[6];

static void lava_build(void) {
    canvas_build(picture(PIC_GLOW, 466, 1.5f, 0x7A2A18, 0.55f), 233, 290);
    for (int i = 0; i < 6; i++) s_blob_pic[i] = picture(i % 2 ? PIC_BLOB_B : PIC_BLOB_A, 2 * (int)BLOBS[i][0], 0, 0, 1);
}

static float lava_frame(const orn_frame_t *f) {
    const float boost = fade(f->poke, 0.7f, 5);
    // A shake splits them: they shrink, then grow back. Only then are they scaled (a scaled picture costs more).
    const float split = f->shake < 8 ? 1 - 0.45f * expf(-f->shake * 0.6f) : 1;
    const bool scaled = split < 0.995f;
    canvas_begin();
    for (int i = 0; i < 6; i++) {
        if (!s_blob_pic[i]) continue;
        // Up slowly, down slowly, across a little; each sways a little as it goes.
        const float k = 0.5f - 0.5f * cosf(TAU * f->t / BLOBS[i][1] + BLOBS[i][2]);
        const float y = 372 - k * 270 + 6 * sinf(f->t * 0.7f + i * 1.3f);
        const float half = sqrtf(fmaxf(0, 200 * 200 - (y - CX) * (y - CX))) - BLOBS[i][0] * 0.5f;
        const float x = CX + BLOBS[i][3] * half + 0.25f * half * sinf(f->t * 0.19f + i) + f->tilt * 40;
        const float opa = 1, r = BLOBS[i][0];  // drawn at 0.85 already; a touch brightens the panel (the light below)
        // Screened, as in the design: where blobs meet they glow lighter, without burning out to white.
        if (scaled) canvas_light_turned(i, s_blob_pic[i], r, r, split, split, 0, x, y, opa, OD_SCREEN);
        else canvas_light(i, s_blob_pic[i], x, y, opa, OD_SCREEN);
    }
    canvas_end();
    return clamp01(0.88f + 0.08f * sinf(f->t * 0.4f) + 0.2f * boost);
}

// ---------------------------------------------------------------- 4 烛火

static const lv_image_dsc_t *s_candle_glow;
static lv_obj_t *s_spark[3];
static line_t s_smoke;

static void candle_build(void) {
    s_candle_glow = picture(PIC_GLOW, 466, 1.6f, 0xFFB45A, 0.75f);
    canvas_build(s_candle_glow, 233, 250);
    s_spark[0] = add_dot(2.2f, 0xFFD27A);
    s_spark[1] = add_dot(1.8f, 0xFFE09A);
    s_spark[2] = add_dot(2.4f, 0xFFB45A);
    line_new(&s_smoke, 4, 0xCFC6B3, 1);
    lv_obj_set_style_line_rounded(s_smoke.o, false, 0);  // it fades frame by frame: no round caps (see line_new)
}

static float candle_frame(const orn_frame_t *f) {
    // A shake blows it out; a touch, or nine seconds, lights it again.
    const bool out = f->shake < f->poke && f->shake < 9;
    const float lit_for = out ? 0 : minf(f->poke, f->shake >= 9 ? f->shake - 9 : 1e6f);
    const float flare = !out && lit_for < 1.5f ? expf(-lit_for * 2) : 0;
    const float grow = out ? 0 : minf(1, 0.2f + lit_for * 1.6f);
    const float flick = 0.5f * sinf(f->t * 9.1f) + 0.3f * sinf(f->t * 13.7f + 1.3f) + 0.2f * sinf(f->t * 4.3f + 0.4f);
    const float lean = -f->tilt * 16 + 2.5f * sinf(f->t * 1.7f) + 1.2f * flick;
    canvas_set_bg(out ? NULL : s_candle_glow);
    canvas_begin();
    if (!out) {
        // The flame stands on its foot (68, 222 of the picture) at (233, 340), and leans and flickers about it.
        canvas_light_turned(0, &s_flame, 68, 222, (1 - 0.025f * flick + 0.08f * flare) * grow,
                            (1 + 0.05f * flick + 0.2f * flare) * grow, lean, 233, 340, 1, OD_ADD);
    }
    canvas_end();
    // A few sparks lift off the tip now and then.
    for (int i = 0; i < 3; i++) {
        const float k = wrapf(f->t / (3.2f + i * 0.9f) + i * 0.37f, 1);
        const float x = CX + 18 * sinf(f->t * 1.3f + i * 2) + (k * 30 - 15) * (i - 1) - f->tilt * 30 * k;
        dot_at(s_spark[i], x, 140 - k * 110, out ? 0 : 0.9f * sinf((float)M_PI * k) * (k < 0.6f ? 1 : (1 - k) * 2.5f));
    }
    // Blown out: a wisp of smoke rises and thins.
    const bool smoke = out && f->shake < 4;
    lv_obj_set_hidden(s_smoke.o, !smoke);
    if (smoke) {
        float xy[18];
        const float up = f->shake * 22;
        for (int j = 0; j < 9; j++) {
            xy[2 * j] = CX + 10 * sinf(j * 0.9f + f->t * 2.2f) * (j / 8.0f + 0.3f);
            xy[2 * j + 1] = 330 - up - j * 22;
        }
        line_set(&s_smoke, xy, 9);
        lv_obj_set_style_line_opa(s_smoke.o, opa_of(0.55f * (1 - f->shake / 4)), 0);
    }
    return out ? 0.45f : clamp01(0.82f + 0.12f * flick + 0.15f * flare);
}

// ---------------------------------------------------------------- 5 月亮

static const float STARS4[4][3] = {{44, 150, 1.8f}, {410, 120, 1.6f}, {420, 330, 1.8f}, {60, 350, 1.5f}};
static lv_obj_t *s_star[10];

static void moon_build(void) {
    // The moon of today, by the clock (around new moon a slim crescent); a clock never set gets a waxing gibbous moon.
    const time_t now = time(NULL);
    const float phase = now >= YK_APP_TIME_VALID_S ? od_moon_shown(od_moon_phase((int64_t)now)) : 0.38f;
    if (fabsf(phase - s_moon_phase) > 0.004f) {
        od_moon(s_moon_px, (uint8_t *)s_moon_px + MOON * MOON * 2, MOON, phase);
        s_moon_phase = phase;
        ESP_LOGI(TAG, "moon phase %.3f", phase);
    }
    add_base_glow(466, 1.4f, 0xFFF3D6, 0.4f, 233, 233);
    for (int i = 0; i < 4; i++) s_star[i] = add_dot(STARS4[i][2], YK_CREAM);
    lv_obj_t *moon = add_image(&s_moon, false);  // over the glow: its dark side hides it, as in the design
    image_at(moon, &s_moon, 233, 233);
}

static float moon_frame(const orn_frame_t *f) {
    for (int i = 0; i < 4; i++) {
        dot_at(s_star[i], STARS4[i][0], STARS4[i][1], 0.2f + 0.6f * powf(fmaxf(0, sinf(f->t * (0.5f + i * 0.17f) + i * 1.9f)), 3));
    }
    return clamp01(0.88f + 0.08f * sinf(f->t * 0.5f) + 0.25f * fade(f->poke, 0.9f, 4));
}

// ---------------------------------------------------------------- 6 水母

static const float MOTES[6][2] = {{90, 0.0f}, {150, 0.3f}, {330, 0.55f}, {380, 0.15f}, {120, 0.75f}, {300, 0.9f}};
static lv_obj_t *s_mote[6];
static line_t s_tent[6], s_arm[2];

static void jelly_build(void) {
    static const float mote_r[6] = {1.6f, 1.2f, 1.8f, 1.3f, 1.5f, 1.2f};
    canvas_build(picture(PIC_GLOW, 380, 1.4f, 0xF6B9A2, 0.45f), 233, 200);  // the bell is the light
    for (int i = 0; i < 6; i++) s_mote[i] = add_dot(mote_r[i], YK_CREAM);
    for (int i = 0; i < 6; i++) line_new(&s_tent[i], 2, 0xF6C2AE, 0.55f);
    line_new(&s_arm[0], 7, 0xF4A68E, 0.7f);
    line_new(&s_arm[1], 6, 0xF4A68E, 0.6f);
}

/** A tentacle or an arm: hangs from x0 across, just above the bell's scalloped rim, swaying more towards its tip.
 *  (They are drawn over the bell now, so they start lower than the design's 44, where the bell no longer covers.) */
static void wave(line_t *l, float bx, float by, float x0, float len, int steps, float amp, float t, float phase, float sx,
                 float sy) {
    float xy[2 * LINE_PTS];
    for (int j = 0; j <= steps; j++) {
        xy[2 * j] = bx + x0 * sx + amp * ((float)j / steps) * sinf(t * 1.4f - j * 0.45f + phase);
        xy[2 * j + 1] = by + 52 * sy + j * len;
    }
    line_set(l, xy, steps + 1);
}

static float jelly_frame(const orn_frame_t *f) {
    const float kick = f->poke < 2.5f ? expf(-f->poke * 1.6f) * sinf(minf((float)M_PI, f->poke * 3)) : 0;
    const float curl = f->shake < 4 ? 1 - 0.6f * expf(-f->shake * 0.8f) : 1;
    // One beat every 3.6 s: the bell narrows and lengthens, and the body rises a little with it.
    const float beat = wrapf(f->t, 3.6f) / 3.6f;
    const float k = minf(1.4f, 0.5f - 0.5f * cosf(TAU * beat) + 0.8f * kick);
    const float sx = 1 - 0.13f * k, sy = 1 + 0.07f * k;
    const float x = CX + f->tilt * 60 + 14 * sinf(f->t / 6.5f);
    const float y = 172 - 16 * sinf(TAU * beat - 0.9f) + 8 * sinf(f->t / 9) - 30 * kick;
    for (int i = 0; i < 6; i++) wave(&s_tent[i], x, y, -60 + i * 24, 14 * curl, 10, 16 * curl, f->t, i * 0.9f, sx, sy);
    wave(&s_arm[0], x, y, -9, 11 * curl, 9, 9, f->t * 0.8f, 2.0f, 1, sy);
    wave(&s_arm[1], x, y, 9, 10 * curl, 9, 9, f->t * 0.8f, 3.4f, 1, sy);
    // The bell beats about the design's origin, (86, 64) of the picture.
    canvas_begin();
    canvas_light_turned(0, &s_bell, 86, 64, sx, sy, 0, x, y, 1, OD_ADD);
    canvas_end();
    for (int i = 0; i < 6; i++) {
        dot_at(s_mote[i], MOTES[i][0] + 8 * sinf(f->t * 0.3f + i), wrapf(440 - (f->t * 6 + MOTES[i][1] * 440), 440) + 13, 0.4f);
    }
    return 1;
}

// ---------------------------------------------------------------- 7 悬挂平衡

static line_t s_hook, s_rod1, s_str_a, s_str_b, s_rod2, s_str_c, s_str_d;
static lv_obj_t *s_ball, *s_semi_obj, *s_tri_obj, *s_glow_a, *s_glow_c, *s_glow_d;

static void mobile_build(void) {
    s_glow_a = add_glow(168, 1.6f, YK_SAGE, 0.3f, 0, 0);
    s_glow_c = add_glow(152, 1.6f, YK_AMBER, 0.3f, 0, 0);
    s_glow_d = add_glow(140, 1.6f, YK_CORAL, 0.3f, 0, 0);
    line_new(&s_hook, 2, 0xD8CFBC, 1);
    line_new(&s_rod1, 3, 0xEDE4D0, 1);
    line_new(&s_str_a, 2, 0xD8CFBC, 1);
    line_new(&s_str_b, 2, 0xD8CFBC, 1);
    line_new(&s_rod2, 3, 0xEDE4D0, 1);
    line_new(&s_str_c, 2, 0xD8CFBC, 1);
    line_new(&s_str_d, 2, 0xD8CFBC, 1);
    s_ball = add_dot(52, 0xA9C094);
    s_semi_obj = add_image(&s_semi, false);
    s_tri_obj = add_image(&s_tri, false);
}

typedef struct {
    float x, y;
} pt_t;

static pt_t rot(pt_t p, float deg) {
    const float a = deg * (float)M_PI / 180, c = cosf(a), s = sinf(a);
    return (pt_t){p.x * c - p.y * s, p.x * s + p.y * c};
}

static pt_t add(pt_t a, pt_t b) { return (pt_t){a.x + b.x, a.y + b.y}; }

static void segment(line_t *l, pt_t a, pt_t b) {
    const float xy[4] = {a.x, a.y, b.x, b.y};
    line_set(l, xy, 2);
}

static float mobile_frame(const orn_frame_t *f) {
    // A touch is a soft push; a shake a bigger one. Both die away like a real mobile's swing.
    const float p = f->poke, s = f->shake;
    const float push = (p < 12 ? 9 * expf(-p / 2.5f) * sinf(TAU * p / 2.6f) : 0) + (s < 12 ? 18 * expf(-s / 2) * sinf(TAU * s / 1.8f) : 0);
    const float push2 = (p < 12 ? 13 * expf(-p / 2.5f) * sinf(TAU * (p - 0.4f) / 2.1f) : 0) + (s < 12 ? 24 * expf(-s / 2) * sinf(TAU * (s - 0.3f) / 1.5f) : 0);
    const float a1 = 3.5f * sinf(TAU * f->t / 10) + push;
    const float a2 = 6 * sinf(TAU * f->t / 6.8f + 1) + push2;
    // Gravity stays down while the screen tilts: the whole mobile turns the other way about its hook.
    const float g = -f->tilt * 9 + 0.8f * sinf(TAU * f->t / 15);
    const pt_t hook = {CX, 0};
    const pt_t c1 = add(hook, rot((pt_t){0, 112}, g));
    const pt_t a_end = add(c1, rot((pt_t){-140, 0}, g + a1)), b_end = add(c1, rot((pt_t){110, 0}, g + a1));
    const pt_t ball = add(a_end, rot((pt_t){0, 172}, g));
    const pt_t c2 = add(b_end, rot((pt_t){0, 60}, g));
    const pt_t c_end = add(c2, rot((pt_t){-96, 0}, g + a2)), d_end = add(c2, rot((pt_t){60, 0}, g + a2));
    segment(&s_hook, add(hook, rot((pt_t){0, -10}, g)), c1);
    segment(&s_rod1, a_end, b_end);
    segment(&s_str_a, a_end, add(a_end, rot((pt_t){0, 120}, g)));
    segment(&s_str_b, b_end, c2);
    segment(&s_rod2, c_end, d_end);
    segment(&s_str_c, c_end, add(c_end, rot((pt_t){0, 70}, g)));
    segment(&s_str_d, d_end, add(d_end, rot((pt_t){0, 56}, g)));
    dot_at(s_ball, ball.x, ball.y, 1);
    const pt_t semi = add(c_end, rot((pt_t){0, 95}, g)), tri = add(d_end, rot((pt_t){0, 94}, g));
    image_at(s_semi_obj, &s_semi, semi.x, semi.y);
    image_at(s_tri_obj, &s_tri, tri.x, tri.y);
    lv_obj_set_pos(s_glow_a, (int32_t)lroundf(ball.x - 84), (int32_t)lroundf(ball.y - 84));
    lv_obj_set_pos(s_glow_c, (int32_t)lroundf(semi.x - 76), (int32_t)lroundf(semi.y + 8 - 76));
    lv_obj_set_pos(s_glow_d, (int32_t)lroundf(tri.x - 70), (int32_t)lroundf(tri.y - 70));
    return 1;
}

// ---------------------------------------------------------------- 8 涟漪

#define RINGS 5
static const float DROPS[4][2] = {{132, 150}, {330, 176}, {312, 326}, {150, 318}};
static lv_obj_t *s_drop_dot;

static void ripple_build(void) {
    // The rings are the canvas's (LVGL drawing five screen-wide circles each frame took 80 ms).
    canvas_build(picture(PIC_GLOW, 320, 1.5f, YK_CREAM, 0.45f), 233, 233);
    s_drop_dot = add_dot(3, 0xFFF6E0);
    lv_obj_t *stone = add_image(&s_stone, false);
    image_at(stone, &s_stone, 233, 233);
}

/** Ring light i as the design gives it: outer radius r, the line `width` thick inside it, shown when it can be seen. */
static void ripple_ring(int i, float cx, float cy, float r, float width, uint32_t rgb, float opa) {
    if (width < 1) width = 1;
    if (opa > 0.02f && r > 1) canvas_ring(i, cx, cy, r - width / 2, width, rgb, opa);
}

static float ripple_frame(const orn_frame_t *f) {
    const float P = 3.2f, L = 16, every = 5.5f;
    const float stir = f->shake < 5 ? 1 + 1.5f * expf(-f->shake) : 1;
    const float cx = CX + f->tilt * 18;
    canvas_begin();
    // A ring is born every 3.2 s and fades out over 16 s as it widens to the rim.
    for (int k = 0; k < RINGS; k++) {
        const float a = wrapf(f->t + k * P, L) / L;
        ripple_ring(k, cx, CX, 30 + a * 196, 3.4f - a * 2.2f, YK_CREAM, minf(1, 0.75f * stir) * powf(1 - a, 1.3f));
    }
    for (int i = 0; i < 2; i++) {
        const float a = f->poke - i * 0.5f;
        if (a >= 0 && a < 6) ripple_ring(RINGS + i, cx, CX, 30 + a * 40, i ? 2.2f : 3.4f, 0xF0B45E, 0.9f * (1 - a / 6));
    }
    // The odd raindrop: a bright point, then two small rings that open and fade.
    const int n = (int)floorf(f->t / every);
    const float age = f->t - n * every;
    const float dx = DROPS[n % 4][0] + f->tilt * 18, dy = DROPS[n % 4][1];
    dot_at(s_drop_dot, dx, dy, age < 0.4f ? 1 - age / 0.4f : 0);
    ripple_ring(RINGS + 2, dx, dy, 4 + age * 16, 2, YK_CREAM, fmaxf(0, 0.6f * (1 - age / 4)));
    ripple_ring(RINGS + 3, dx, dy, fmaxf(2, 4 + (age - 0.5f) * 16), 1.4f, YK_CREAM,
                age > 0.5f ? fmaxf(0, 0.45f * (1 - (age - 0.5f) / 4)) : 0);
    canvas_end();
    // The glow swells as each ring is born, and with a touch: by the panel's level, not the glow's (see add_base_glow).
    return clamp01(0.84f + 0.1f * sinf(TAU * f->t / P) + 0.25f * fade(f->poke, 1, 3));
}

// ---------------------------------------------------------------- 9 极光

static float s_flow;

static void aurora_build(void) {
    static const float stars[10][2] = {{80, 200}, {130, 96}, {196, 62}, {262, 44}, {332, 74},
                                       {388, 140}, {408, 236}, {60, 288}, {160, 300}, {350, 270}};
    for (int b = 0; b < 2; b++) {
        memset(s_buf[b], 0, SCREEN * SCREEN * 2);
        s_rows[b][0] = 1, s_rows[b][1] = 0;
    }
    s_shown = 0;
    s_screen.data = (const uint8_t *)s_buf[0];
    s_canvas_obj = add_image(&s_screen, false);
    lv_obj_set_pos(s_canvas_obj, 0, 0);
    for (int i = 0; i < 10; i++) {
        s_star[i] = add_dot(1.3f, YK_CREAM);
        dot_at(s_star[i], stars[i][0], stars[i][1], 0.3f);
    }
    s_flow = 0;
}

static float aurora_frame(const orn_frame_t *f) {
    // The flow runs left to right; tilting slows it, stops it or turns it round.
    s_flow += (0.3f + f->tilt * 0.45f) * f->dt;
    const float shimmer = fade(f->poke, 1.2f, 3);
    const float opa[3] = {minf(1, 0.85f + 0.15f * sinf(f->t * 0.31f) + 0.3f * shimmer),
                          minf(1, 0.8f + 0.2f * sinf(f->t * 0.27f + 1) + 0.3f * shimmer),
                          minf(1, 0.75f + 0.2f * sinf(f->t * 0.23f + 2) + 0.3f * shimmer)};
    // The helper draws it (see the canvas): show the last one, hand over this one.
    collect();
    s_job.aurora = true;
    s_job.target = !s_shown;
    s_job.flow = s_flow;
    memcpy(s_job.opa, opa, sizeof(opa));
    s_busy = true;
    xSemaphoreGive(s_go);
    for (int i = 0; i < 10; i++) {
        const lv_opa_t k = opa_of(0.2f + 0.6f * powf(fmaxf(0, sinf(f->t * (0.45f + i * 0.11f) + i * 2.3f)), 3));
        if (lv_obj_get_style_opa(s_star[i], 0) != k) lv_obj_set_style_opa(s_star[i], k, 0);
    }
    return 1;
}

// ---------------------------------------------------------------- 10 光环

#define HALF 20
static line_t s_back[2], s_front[2];
static lv_obj_t *s_moon_back, *s_moon_front;
static float s_spin;

static void halo_build(void) {
    add_base_glow(466, 1.6f, 0xF2B062, 0.55f, 233, 233);
    line_new(&s_back[0], 3, 0xF7EAD0, 0.35f);
    line_new(&s_back[1], 5, 0xF0B45E, 0.3f);
    s_moon_back = add_dot(9, 0xF7EAD0);
    lv_obj_t *orb = add_image(&s_orb, false);
    image_at(orb, &s_orb, 233, 233);
    line_new(&s_front[0], 3, 0xF7EAD0, 0.9f);
    line_new(&s_front[1], 5, 0xF0B45E, 0.8f);
    s_moon_front = add_dot(9, 0xF7EAD0);
    s_spin = 0;
}

/** Half a ring, rx by ry, turned by theta degrees: the far half (from angle pi to 2 pi) or the near one. */
static void half_ring(line_t *l, float rx, float ry, float theta, bool near) {
    float xy[2 * (HALF + 1)];
    const float a = theta * (float)M_PI / 180, c = cosf(a), s = sinf(a);
    for (int i = 0; i <= HALF; i++) {
        const float u = (near ? 0 : (float)M_PI) + (float)M_PI * i / HALF;
        const float lx = rx * cosf(u), ly = ry * sinf(u);
        xy[2 * i] = CX + lx * c - ly * s;
        xy[2 * i + 1] = CX + lx * s + ly * c;
    }
    line_set(l, xy, HALF + 1);
}

static float halo_frame(const orn_frame_t *f) {
    static const float rx[2] = {150, 205}, base[2] = {-16, 8}, speed[2] = {5, -2.8f};
    s_spin += (1 + 3 * fade(f->poke, 0.9f, 4)) * f->dt;
    const float wob = f->shake < 5 ? 0.12f * expf(-f->shake * 0.8f) * sinf(f->shake * 9) : 0;
    // Tilting the screen changes how flat the rings look.
    const float incl = fmaxf(0.1f, minf(0.5f, 0.28f - f->tilt * 0.14f + wob));
    float theta[2];
    for (int k = 0; k < 2; k++) {
        theta[k] = base[k] + speed[k] * s_spin;
        half_ring(&s_back[k], rx[k], rx[k] * incl, theta[k], false);
        half_ring(&s_front[k], rx[k], rx[k] * incl, theta[k], true);
    }
    // One small moon on the outer ring; behind the planet it dims.
    const float phi = s_spin * 0.35f, lx = rx[1] * cosf(phi), ly = rx[1] * incl * sinf(phi);
    const float a = theta[1] * (float)M_PI / 180;
    const float mx = CX + lx * cosf(a) - ly * sinf(a), my = CX + lx * sinf(a) + ly * cosf(a);
    const bool front = sinf(phi) >= 0;
    dot_at(s_moon_back, mx, my, front ? 0 : 0.5f);
    dot_at(s_moon_front, mx, my, front ? 1 : 0);
    return clamp01(0.9f + 0.06f * sinf(f->t * 0.5f) + 0.2f * fade(f->poke, 1.2f, 3));
}

// ---------------------------------------------------------------- the table

static const struct {
    const char *key;
    void (*build)(void);
    float (*frame)(const orn_frame_t *f);
} SCENES[ORN_COUNT] = {
    {"mushroom", mushroom_build, mushroom_frame}, {"fireflies", fireflies_build, fireflies_frame},
    {"lava", lava_build, lava_frame},             {"candle", candle_build, candle_frame},
    {"moon", moon_build, moon_frame},             {"jelly", jelly_build, jelly_frame},
    {"mobile", mobile_build, mobile_frame},       {"ripple", ripple_build, ripple_frame},
    {"aurora", aurora_build, aurora_frame},       {"halo", halo_build, halo_frame},
};

void orn_scene_build(int i, lv_obj_t *root) {
    collect();  // the helper done with the pictures, before the next scene takes them
    s_root = root;
    s_scene = i >= 0 && i < ORN_COUNT ? i : 0;
    lv_obj_clean(root);
    SCENES[s_scene].build();
}

float orn_scene_frame(const orn_frame_t *f) { return SCENES[s_scene].frame(f); }

const char *orn_scene_key(int i) { return i >= 0 && i < ORN_COUNT ? SCENES[i].key : "?"; }
