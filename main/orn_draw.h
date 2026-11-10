// The pictures the scenes are made of, worked out in plain C (no LVGL, no ESP-IDF) into buffers the caller owns:
// soft glows, a firefly, blobs of light, lit spheres, the moon at a phase, flat shapes and the aurora. RGB565 is
// little endian and dithered (no bands on the 16-bit panel); ARGB8888 is 0xAARRGGBB; A8 is alpha only.
#pragma once

#include <stdint.h>

/** A round glow of colour rgb on black (RGB565, drawn additively), size x size: `strength` in the middle falling to
 *  0 at the rim as (1 - d)^power, d the distance over the radius. Dithered, as a soft glow on a 16-bit panel shows
 *  rings otherwise. */
void od_glow(uint16_t *px, int size, float power, uint32_t rgb, float strength);
/** A firefly of colour rgb on black (RGB565): a small hot core in a wide soft halo, size x size. */
void od_firefly(uint16_t *px, int size, uint32_t rgb);
/** A blob of light on black for additive drawing (RGB565): hot in the middle, then mid, rim, black at the edge. */
void od_blob(uint16_t *px, int size, uint32_t hot, uint32_t mid, uint32_t rim);
/** A lit sphere, light from the upper left, with an anti-aliased rim (ARGB8888), size x size. */
void od_sphere(uint32_t *px, int size, uint32_t light, uint32_t mid, uint32_t dark);
/** The moon (RGB565), size x size: lit as at phase p (0 new, 0.25 first quarter, 0.5 full), its dark side faintly lit
 *  by the earth, a few soft craters. With `alpha` (size x size, the disc's cover) the colours are the disc's own, for
 *  drawing over a glow (RGB565A8: px then alpha); without, the rim is faded into black, for drawing on black. */
void od_moon(uint16_t *px, uint8_t *alpha, int size, float phase);
/** Where the moon is in its month at unix_s (UTC): 0 new, 0.5 full. */
float od_moon_phase(int64_t unix_s);
/** The phase to draw: the real one, except that around new moon (when the real moon is a dark disc) a slim crescent
 *  on the side it is waxing or waning to, so the ornament never goes dark. */
float od_moon_shown(float phase);
/** A triangle, apex at the top middle, base along the bottom, anti-aliased (ARGB8888), w x h. */
void od_triangle(uint32_t *px, int w, int h, uint32_t rgb);
/** The upper half of a disc, flat side down, anti-aliased (ARGB8888): 2r x r. */
void od_half_disc(uint32_t *px, int r, uint32_t rgb);
/** The aurora, w x h RGB565 (w at most 466; the design's 466 units across it): three soft bands drifting with the
 *  phase f, each at its own strength opa[i] (0..1). Only the rows the bands cross are drawn. `rows` NULL clears the
 *  rest of the buffer; otherwise it holds the rows drawn last time ({first, last}, first > last for none) and gets
 *  the rows drawn now, and only rows drawn before and not now are cleared. */
void od_aurora(uint16_t *px, int w, int h, float f, const float opa[3], int rows[2]);
/** A picture moved, scaled and turned, drawn into dst (dw x dh RGB565, cleared to black): the source point (px, py)
 *  lands at (ax, ay), the picture scaled sx across and sy down about it, then turned `deg` degrees clockwise (as LVGL
 *  turns). Bilinear; outside the source is black. Far cheaper than LVGL turning it while it draws. */
void od_transform(uint16_t *dst, int dw, int dh, const uint16_t *src, int sw, int sh, float px, float py, float sx,
                  float sy, float deg, float ax, float ay);
/** How a light goes onto what is there: added (each channel held at its top), or screened (1 - (1 - d)(1 - s): two
 *  lights meeting get lighter but not white so soon; the design's lava). */
typedef enum { OD_ADD, OD_SCREEN } od_blend_t;
/** As od_transform, but onto what is in dst (at opa / 256, 256 as it is), nothing cleared. `box` (may be NULL) gets
 *  the pixels it may have touched, {x0, y0, x1, y1} inclusive; x0 > x1 when none. */
void od_transform_add(uint16_t *dst, int dw, int dh, const uint16_t *src, int sw, int sh, float px, float py,
                      float sx, float sy, float deg, float ax, float ay, int opa, od_blend_t mode, int box[4]);
/** src (sw x sh RGB565, a light on black) onto dst (dw x dh) with its top left at (x, y), at opa / 256 (256 as it
 *  is). `box` (may be NULL) as for od_transform_add. */
void od_add(uint16_t *dst, int dw, int dh, const uint16_t *src, int sw, int sh, int x, int y, int opa, od_blend_t mode,
            int box[4]);
/** Puts back the box {x0, y0, x1, y1} of dst from the background bg (bw x bh) whose top left is at (bx, by); black
 *  where it does not reach, or everywhere when bg is NULL. */
void od_restore(uint16_t *dst, int dw, int dh, const uint16_t *bg, int bw, int bh, int bx, int by, const int box[4]);
/** A ring of radius r (to the middle of its line) about (cx, cy), `width` thick, anti-aliased, drawn over dst in rgb
 *  at opa (0..1). `box` (may be NULL) gets the pixels it touched, as for od_transform_add. Only the ring's own
 *  pixels are visited. */
void od_ring(uint16_t *dst, int dw, int dh, float cx, float cy, float r, float width, uint32_t rgb, float opa,
             int box[4]);
/** Puts back from the background (as od_restore) exactly the pixels od_ring with the same ring may have touched. */
void od_ring_restore(uint16_t *dst, int dw, int dh, const uint16_t *bg, int bw, int bh, int bx, int by, float cx,
                     float cy, float r, float width);
uint16_t od_rgb565(uint32_t rgb, int x, int y);
