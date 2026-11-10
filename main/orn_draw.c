#include "orn_draw.h"

#include <math.h>
#include <string.h>

static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static int dither(int v, int bits, int threshold) {
    const int drop = 8 - bits;
    const int out = (v + (threshold << drop >> 4)) >> drop;
    return out > (1 << bits) - 1 ? (1 << bits) - 1 : out;
}

uint16_t od_rgb565(uint32_t rgb, int x, int y) {
    const int t = BAYER[y & 3][x & 3];
    const int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return (uint16_t)(dither(r, 5, t) << 11 | dither(g, 6, t) << 5 | dither(b, 5, t));
}

static uint32_t mix(uint32_t a, uint32_t b, float k) {
    k = clampf(k, 0, 1);
    uint32_t out = 0;
    for (int s = 0; s <= 16; s += 8) {
        const float ca = (a >> s) & 0xFF, cb = (b >> s) & 0xFF;
        out |= (uint32_t)(ca + (cb - ca) * k + 0.5f) << s;
    }
    return out;
}

static uint32_t scale_rgb(uint32_t c, float k) { return mix(0, c, k); }

/** Three stops at 0, m, 1 of t. */
static uint32_t ramp3(uint32_t c0, uint32_t c1, uint32_t c2, float m, float t) {
    return t < m ? mix(c0, c1, t / m) : mix(c1, c2, (t - m) / (1 - m));
}

#define FALLOFF 512  // steps of the squared distance in the tables below: no root or power per pixel

/** A radial picture from a table of the light against the squared distance (0 middle .. FALLOFF rim). */
static void radial(uint16_t *px, int size, uint32_t rgb, const float *k) {
    const float c = (size - 1) / 2.0f, r2 = (size / 2.0f) * (size / 2.0f);
    const int R = (rgb >> 16) & 0xFF, G = (rgb >> 8) & 0xFF, B = rgb & 0xFF;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            const float q = ((x - c) * (x - c) + (y - c) * (y - c)) / r2;
            const float v = q >= 1 ? 0 : k[(int)(q * FALLOFF)];
            px[y * size + x] = od_rgb565((uint32_t)(R * v) << 16 | (uint32_t)(G * v) << 8 | (uint32_t)(B * v), x, y);
        }
    }
}

void od_glow(uint16_t *px, int size, float power, uint32_t rgb, float strength) {
    float k[FALLOFF + 1];
    for (int i = 0; i <= FALLOFF; i++) k[i] = powf(1 - sqrtf((float)i / FALLOFF), power) * strength;
    radial(px, size, rgb, k);
}

void od_firefly(uint16_t *px, int size, uint32_t rgb) {
    // The design's CSS stops, over half the width: solid to 0.23, half at 0.37, 0.14 at 0.65, none at 0.93.
    static const float at[] = {0.0f, 0.23f, 0.37f, 0.65f, 0.93f}, alpha[] = {1.0f, 1.0f, 0.5f, 0.14f, 0.0f};
    float k[FALLOFF + 1];
    for (int i = 0; i <= FALLOFF; i++) {
        const float d = sqrtf((float)i / FALLOFF);
        k[i] = 0;
        for (int s = 0; s < 4; s++) {
            if (d >= at[s] && d < at[s + 1]) k[i] = alpha[s] + (alpha[s + 1] - alpha[s]) * (d - at[s]) / (at[s + 1] - at[s]);
        }
    }
    radial(px, size, rgb, k);
}

void od_blob(uint16_t *px, int size, uint32_t hot, uint32_t mid, uint32_t rim) {
    const float cx = (size - 1) / 2.0f, cy = size * 0.42f, r = size / 2.0f;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            // The light's centre sits a little high, as in the design (cy 40%); it fades to black at the rim.
            const float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) / r;
            const float edge = sqrtf((x - cx) * (x - cx) + (y - (size - 1) / 2.0f) * (y - (size - 1) / 2.0f)) / r;
            uint32_t c = d < 0.4f ? mix(hot, mid, d / 0.4f) : d < 0.78f ? mix(mid, rim, (d - 0.4f) / 0.38f) : scale_rgb(rim, 0.8f * (1 - (d - 0.78f) / 0.22f));
            if (edge >= 1) c = 0;
            else if (edge > 0.85f) c = scale_rgb(c, (1 - edge) / 0.15f);
            px[y * size + x] = od_rgb565(c, x, y);
        }
    }
}

void od_sphere(uint32_t *px, int size, uint32_t light, uint32_t mid, uint32_t dark) {
    const float c = (size - 1) / 2.0f, r = size / 2.0f;
    const float hx = size * 0.36f, hy = size * 0.32f, reach = size * 0.74f;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            const float d = sqrtf((x - c) * (x - c) + (y - c) * (y - c));
            const float cover = clampf(r - d + 0.5f, 0, 1);
            const float t = sqrtf((x - hx) * (x - hx) + (y - hy) * (y - hy)) / reach;
            const uint32_t rgb = ramp3(light, mid, dark, 0.5f, clampf(t, 0, 1));
            px[y * size + x] = (uint32_t)(cover * 255 + 0.5f) << 24 | rgb;
        }
    }
}

float od_moon_phase(int64_t unix_s) {
    const double synodic = 29.530588853 * 86400.0, new_moon = 947182440.0;  // 2000-01-06 18:14 UTC
    double p = fmod(((double)unix_s - new_moon) / synodic, 1.0);
    if (p < 0) p += 1;
    return (float)p;
}

#define MOON_SLIMMEST 0.1f  // about a tenth of the disc lit

float od_moon_shown(float phase) {
    if (phase < MOON_SLIMMEST) return MOON_SLIMMEST;          // just past new: waxing
    if (phase > 1 - MOON_SLIMMEST) return 1 - MOON_SLIMMEST;  // just before new: waning
    return phase;
}

void od_moon(uint16_t *px, uint8_t *alpha, int size, float phase) {
    static const float craters[][3] = {{-0.35f, -0.26f, 0.15f}, {0.25f, -0.44f, 0.09f}, {0.38f, 0.15f, 0.18f},
                                       {-0.17f, 0.35f, 0.11f}, {0.07f, -0.05f, 0.07f}, {-0.56f, 0.1f, 0.08f}};
    const float c = (size - 1) / 2.0f, r = size / 2.0f - 1;
    // Where the sun is: behind the moon at new (nothing lit), to the right at first quarter, in front at full.
    const float a = 2 * (float)M_PI * phase, sx = sinf(a), sz = -cosf(a);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            const float nx = (x - c) / r, ny = (y - c) / r, rr = nx * nx + ny * ny;
            const float cover = clampf((1 - sqrtf(rr)) * r + 0.5f, 0, 1);
            if (cover <= 0) {
                px[y * size + x] = 0;
                if (alpha) alpha[y * size + x] = 0;
                continue;
            }
            const float nz = sqrtf(clampf(1 - rr, 0, 1));
            const float sun = nx * sx + nz * sz;
            const float lit = clampf((sun + 0.04f) / 0.12f, 0, 1);  // a soft terminator
            const float hx = nx + 0.16f, hy = ny + 0.28f;
            uint32_t day = ramp3(0xFFFDF5, 0xF7E8C8, 0xDEC497, 0.65f, clampf(sqrtf(hx * hx + hy * hy) / 1.4f, 0, 1));
            for (size_t i = 0; i < sizeof(craters) / sizeof(craters[0]); i++) {
                const float dx = nx - craters[i][0], dy = ny - craters[i][1];
                const float k = 1 - sqrtf(dx * dx + dy * dy) / craters[i][2];
                if (k > 0) day = mix(day, 0xD3BE93, clampf(k * 2.5f, 0, 1) * 0.32f);
            }
            const uint32_t night = 0x1E1B17;  // the earthshine on the dark side
            const uint32_t rgb = mix(night, day, lit);
            if (alpha) {
                px[y * size + x] = od_rgb565(rgb, x, y);
                alpha[y * size + x] = (uint8_t)(cover * 255 + 0.5f);
            } else {
                px[y * size + x] = od_rgb565(scale_rgb(rgb, cover), x, y);
            }
        }
    }
}

static float edge_side(float ax, float ay, float bx, float by, float px, float py) {
    // Signed distance of p from the line a -> b: negative on the inside of this (clockwise on screen) triangle.
    const float ex = bx - ax, ey = by - ay, len = sqrtf(ex * ex + ey * ey);
    return ((px - ax) * ey - (py - ay) * ex) / len;
}

void od_triangle(uint32_t *px, int w, int h, uint32_t rgb) {
    const float ax = w / 2.0f, ay = 0, bx = w, by = h, cx = 0, cy = h;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const float qx = x + 0.5f, qy = y + 0.5f;
            // Outside as soon as it is outside any one edge: the largest of the three distances decides.
            float d = edge_side(ax, ay, bx, by, qx, qy);
            const float d2 = edge_side(bx, by, cx, cy, qx, qy), d3 = edge_side(cx, cy, ax, ay, qx, qy);
            if (d2 > d) d = d2;
            if (d3 > d) d = d3;
            px[y * w + x] = (uint32_t)(clampf(0.5f - d, 0, 1) * 255 + 0.5f) << 24 | rgb;
        }
    }
}

void od_half_disc(uint32_t *px, int r, uint32_t rgb) {
    const int w = 2 * r;
    for (int y = 0; y < r; y++) {
        for (int x = 0; x < w; x++) {
            const float dx = x + 0.5f - r, dy = y + 0.5f - r;
            const float cover = clampf(r - sqrtf(dx * dx + dy * dy) + 0.5f, 0, 1);
            px[y * w + x] = (uint32_t)(cover * 255 + 0.5f) << 24 | rgb;
        }
    }
}

typedef struct {
    float speed, base, amp, k, phase, thick;
    uint32_t color;
} band_t;

// The design's three bands (466 wide): sage, gold, rose.
static const band_t BANDS[3] = {
    {1.0f, 96, 34, 0.011f, 0.0f, 120, 0xB5CC9C},
    {1.2f, 182, 30, 0.013f, 1.9f, 104, 0xF0CC80},
    {0.85f, 272, 26, 0.016f, 3.7f, 92, 0xF0A99E},
};

#define AURORA_MAX_W 466

/** The three bands' light at row y (pixels, fractional) for each of `cols` sample columns, as r g b bytes. */
static void sample_row(float y, int cols, const int32_t top8[3][AURORA_MAX_W], const int32_t inv[3][AURORA_MAX_W],
                       const int32_t cr[3], const int32_t cg[3], const int32_t cb[3], const uint8_t *profile,
                       uint8_t *out) {
    const int32_t y8 = (int32_t)(y * 256);
    for (int x = 0; x < cols; x++) {
        int32_t r = 0, g = 0, bl = 0;
        for (int b = 0; b < 3; b++) {
            const int32_t d = y8 - top8[b][x];
            if (d < 0) continue;
            const int32_t u = (d * inv[b][x]) >> 16;
            if (u > 255) continue;
            const int32_t k = profile[u];
            r += cr[b] * k;
            g += cg[b] * k;
            bl += cb[b] * k;
        }
        r >>= 16;
        g >>= 16;
        bl >>= 16;
        out[3 * x] = (uint8_t)(r > 255 ? 255 : r);
        out[3 * x + 1] = (uint8_t)(g > 255 ? 255 : g);
        out[3 * x + 2] = (uint8_t)(bl > 255 ? 255 : bl);
    }
}

void od_aurora(uint16_t *px, int w, int h, float f, const float opa[3], int rows[2]) {
    if (w > AURORA_MAX_W) return;
    // Each band's light down its depth (u 0 top .. 255 bottom): brightest 40% of the way down, nothing at the edges.
    static uint8_t profile[256];
    if (!profile[102]) {
        for (int u = 0; u < 256; u++) {
            const float k = u / 256.0f;
            profile[u] = (uint8_t)(255 * 0.85f * (k < 0.4f ? k / 0.4f : (1 - k) / 0.6f) + 0.5f);
        }
    }
    // Per band and column: where the band starts (rows, 8 fractional bits) and 65536 / its depth in rows. Worked out
    // once a frame, so each pixel is integer sums and a table, row by row. A wide one is worked out on every other
    // row and column (the bands are soft: nothing to see in between) and only dithered pixel by pixel.
    static int32_t top8[3][AURORA_MAX_W], inv[3][AURORA_MAX_W];
    static uint8_t line[2][3 * AURORA_MAX_W];
    int32_t cr[3], cg[3], cb[3];
    const float scale = 466.0f / w;
    const int step = w >= 300 ? 2 : 1, cols = (w + step - 1) / step;
    int lo = h, hi = -1;
    for (int b = 0; b < 3; b++) {
        const band_t *B = &BANDS[b];
        const int32_t o = (int32_t)(clampf(opa[b], 0, 1) * 256 + 0.5f);
        cr[b] = (int32_t)((B->color >> 16) & 0xFF) * o;
        cg[b] = (int32_t)((B->color >> 8) & 0xFF) * o;
        cb[b] = (int32_t)(B->color & 0xFF) * o;
        const float ff = f * B->speed;
        for (int x = 0; x < cols; x++) {
            const float X = (x * step + (step - 1) * 0.5f) * scale;  // in the design's units
            const float top = B->base + B->amp * sinf(B->k * X + ff + B->phase) + B->amp * 0.5f * sinf(B->k * 2.3f * X - ff * 0.7f + B->phase * 1.7f);
            const float th = B->thick * (0.7f + 0.3f * sinf(B->k * 1.4f * X + ff * 0.5f + B->phase));
            top8[b][x] = (int32_t)lroundf(top / scale * 256);
            inv[b][x] = (int32_t)(65536 * scale / th);
            if (o == 0) continue;
            const int y0 = (int)floorf(top / scale), y1 = (int)ceilf((top + th) / scale);
            if (y0 < lo) lo = y0;
            if (y1 > hi) hi = y1;
        }
    }
    if (lo < 0) lo = 0;
    if (hi > h - 1) hi = h - 1;
    if (!rows) {
        memset(px, 0, (size_t)w * h * sizeof(uint16_t));
    } else {
        for (int y = rows[0] < 0 ? 0 : rows[0]; y <= rows[1] && y < h; y++) {
            if (y < lo || y > hi) memset(&px[y * w], 0, (size_t)w * sizeof(uint16_t));
        }
        rows[0] = lo;
        rows[1] = hi;
    }
    // One worked-out row of samples, the colours at 8 bits. Wide: rows lo, lo + 2, ... are worked out, the row
    // between two of them takes their average (rows doubled as they are show as fine stripes across the bands, which
    // change fast downwards), and each sample covers two pixels across (the bands change slowly sideways).
    static uint8_t mid[3 * AURORA_MAX_W];
    int cur = 0;
    sample_row(lo + (step - 1) * 0.5f, cols, top8, inv, cr, cg, cb, profile, line[0]);
    for (int y = lo; y <= hi; y += step) {
        if (step == 2) {
            sample_row(y + 2 + 0.5f, cols, top8, inv, cr, cg, cb, profile, line[!cur]);
            for (int i = 0; i < 3 * cols; i++) mid[i] = (uint8_t)((line[cur][i] + line[!cur][i] + 1) >> 1);
        }
        for (int yy = y; yy < y + step && yy <= hi; yy++) {
            uint16_t *row = &px[yy * w];
            const uint8_t *bayer = BAYER[yy & 3];
            const uint8_t *src = yy == y ? line[cur] : mid;
            for (int x = 0; x < w; x++) {
                const int i = 3 * (step == 2 ? x >> 1 : x);
                const int r = src[i], g = src[i + 1], bl = src[i + 2];
                if (!(r | g | bl)) {
                    row[x] = 0;
                    continue;
                }
                const int t = bayer[x & 3];
                row[x] = (uint16_t)(dither(r, 5, t) << 11 | dither(g, 6, t) << 5 | dither(bl, 5, t));
            }
        }
        if (step == 2) cur = !cur;
        else if (y + 1 <= hi) sample_row(y + 1 + 0.0f, cols, top8, inv, cr, cg, cb, profile, line[cur]);
    }
}

/** The source pixel at (x, y), black outside. */
static inline uint32_t src_at(const uint16_t *src, int sw, int sh, int x, int y) {
    return (unsigned)x < (unsigned)sw && (unsigned)y < (unsigned)sh ? src[y * sw + x] : 0;
}

/** d + s x opa / 256, channel by channel, each held at its top. */
static inline uint16_t add565(uint32_t d, uint32_t s, uint32_t opa) {
    const uint32_t r = (d >> 11) + (((s >> 11) * opa + 128) >> 8);
    const uint32_t g = ((d >> 5) & 0x3F) + ((((s >> 5) & 0x3F) * opa + 128) >> 8);
    const uint32_t b = (d & 0x1F) + (((s & 0x1F) * opa + 128) >> 8);
    return (uint16_t)((r > 31 ? 31 : r) << 11 | (g > 63 ? 63 : g) << 5 | (b > 31 ? 31 : b));
}

/** d screened with s x opa / 256 (1 - (1 - d)(1 - s), channel by channel): lighter where both are light, never past
 *  white, softer than adding where two lights meet. */
static inline uint16_t screen565(uint32_t d, uint32_t s, uint32_t opa) {
    const uint32_t sr = ((s >> 11) * opa + 128) >> 8, sg = ((((s >> 5) & 0x3F) * opa + 128) >> 8),
                   sb = (((s & 0x1F) * opa + 128) >> 8);
    const uint32_t dr = d >> 11, dg = (d >> 5) & 0x3F, db = d & 0x1F;
    // d x s / 31 and / 63 by a multiply and a shift (no division per pixel): 1057 / 32768 ~ 1 / 31, 520 / 32768 ~ 1 / 63.
    const uint32_t r = dr + sr - ((dr * sr * 1057 + 16384) >> 15), g = dg + sg - ((dg * sg * 520 + 16384) >> 15),
                   b = db + sb - ((db * sb * 1057 + 16384) >> 15);
    return (uint16_t)(r << 11 | g << 5 | b);
}

/** Screening at full strength by table: the 5-bit and 6-bit channels' 1 - (1 - d)(1 - s), worked out once. */
static uint8_t s_screen5[32][32], s_screen6[64][64];

static void screen_tables(void) {
    if (s_screen5[31][1]) return;
    for (int d = 0; d < 32; d++) {
        for (int v = 0; v < 32; v++) s_screen5[d][v] = (uint8_t)(d + v - (d * v + 15) / 31);
    }
    for (int d = 0; d < 64; d++) {
        for (int v = 0; v < 64; v++) s_screen6[d][v] = (uint8_t)(d + v - (d * v + 31) / 63);
    }
}

static inline uint16_t blend565(od_blend_t mode, uint32_t d, uint32_t s, uint32_t opa) {
    if (mode != OD_SCREEN) return add565(d, s, opa);
    if (opa < 256) return screen565(d, s, opa);
    return (uint16_t)(s_screen5[d >> 11][s >> 11] << 11 | s_screen6[(d >> 5) & 0x3F][(s >> 5) & 0x3F] << 5 |
                      s_screen5[d & 0x1F][s & 0x1F]);
}

static void set_box(int box[4], int x0, int y0, int x1, int y1) {
    if (!box) return;
    box[0] = x0, box[1] = y0, box[2] = x1, box[3] = y1;
}

void od_add(uint16_t *dst, int dw, int dh, const uint16_t *src, int sw, int sh, int x, int y, int opa, od_blend_t mode,
            int box[4]) {
    const int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    const int x1 = (x + sw < dw ? x + sw : dw) - 1, y1 = (y + sh < dh ? y + sh : dh) - 1;
    if (opa <= 0 || x0 > x1 || y0 > y1) {
        set_box(box, 1, 1, 0, 0);
        return;
    }
    const uint32_t k = opa > 256 ? 256 : (uint32_t)opa;
    if (mode == OD_SCREEN) screen_tables();
    for (int yy = y0; yy <= y1; yy++) {
        const uint16_t *s = &src[(yy - y) * sw + (x0 - x)];
        uint16_t *d = &dst[yy * dw + x0];
        for (int i = 0; i <= x1 - x0; i++) {
            if (s[i]) d[i] = blend565(mode, d[i], s[i], k);
        }
    }
    set_box(box, x0, y0, x1, y1);
}

void od_restore(uint16_t *dst, int dw, int dh, const uint16_t *bg, int bw, int bh, int bx, int by, const int box[4]) {
    const int x0 = box[0] < 0 ? 0 : box[0], x1 = box[2] > dw - 1 ? dw - 1 : box[2];
    const int y0 = box[1] < 0 ? 0 : box[1], y1 = box[3] > dh - 1 ? dh - 1 : box[3];
    if (x0 > x1 || y0 > y1) return;
    // The columns the background covers, within the box.
    const int c0 = x0 > bx ? x0 : bx, c1 = x1 < bx + bw - 1 ? x1 : bx + bw - 1;
    for (int y = y0; y <= y1; y++) {
        uint16_t *d = &dst[y * dw];
        const int sy = y - by;
        if (!bg || sy < 0 || sy >= bh || c0 > c1) {
            memset(&d[x0], 0, (size_t)(x1 - x0 + 1) * sizeof(uint16_t));
            continue;
        }
        if (c0 > x0) memset(&d[x0], 0, (size_t)(c0 - x0) * sizeof(uint16_t));
        memcpy(&d[c0], &bg[sy * bw + (c0 - bx)], (size_t)(c1 - c0 + 1) * sizeof(uint16_t));
        if (x1 > c1) memset(&d[c1 + 1], 0, (size_t)(x1 - c1) * sizeof(uint16_t));
    }
}

void od_transform_add(uint16_t *dst, int dw, int dh, const uint16_t *src, int sw, int sh, float px, float py,
                      float sx, float sy, float deg, float ax, float ay, int opa, od_blend_t mode, int box[4]) {
    set_box(box, 1, 1, 0, 0);
    if (sx <= 0.01f || sy <= 0.01f || opa <= 0) return;
    const uint32_t k = opa > 256 ? 256 : (uint32_t)opa;
    if (mode == OD_SCREEN) screen_tables();
    const float a = deg * (float)M_PI / 180, c = cosf(a), s = sinf(a);
    // Only the box the turned picture covers.
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    const float cx[4] = {0, (float)sw, 0, (float)sw}, cy[4] = {0, 0, (float)sh, (float)sh};
    for (int i = 0; i < 4; i++) {
        const float rx = (cx[i] - px) * sx, ry = (cy[i] - py) * sy;
        const float X = ax + rx * c - ry * s, Y = ay + rx * s + ry * c;
        x0 = fminf(x0, X), y0 = fminf(y0, Y), x1 = fmaxf(x1, X), y1 = fmaxf(y1, Y);
    }
    const int bx0 = x0 < 0 ? 0 : (int)x0, by0 = y0 < 0 ? 0 : (int)y0;
    const int bx1 = x1 > dw - 1 ? dw - 1 : (int)x1 + 1, by1 = y1 > dh - 1 ? dh - 1 : (int)y1 + 1;
    if (bx0 > bx1 || by0 > by1) return;
    // Back from the screen to the source: 16.16 fixed point, stepping along each row.
    const int32_t du = (int32_t)(c / sx * 65536), dv = (int32_t)(-s / sy * 65536);
    for (int y = by0; y <= by1; y++) {
        const float rx = bx0 + 0.5f - ax, ry = y + 0.5f - ay;
        int32_t u = (int32_t)((px + (c * rx + s * ry) / sx - 0.5f) * 65536);
        int32_t v = (int32_t)((py + (-s * rx + c * ry) / sy - 0.5f) * 65536);
        uint16_t *row = &dst[y * dw];
        for (int x = bx0; x <= bx1; x++, u += du, v += dv) {
            const int ix = u >> 16, iy = v >> 16;  // arithmetic shift: -1 just left of the picture
            if (ix < -1 || iy < -1 || ix >= sw || iy >= sh) continue;
            const uint32_t fx = (u >> 8) & 0xFF, fy = (v >> 8) & 0xFF;
            const uint32_t p00 = src_at(src, sw, sh, ix, iy), p10 = src_at(src, sw, sh, ix + 1, iy);
            const uint32_t p01 = src_at(src, sw, sh, ix, iy + 1), p11 = src_at(src, sw, sh, ix + 1, iy + 1);
            if (!(p00 | p10 | p01 | p11)) continue;
            const uint32_t w00 = (256 - fx) * (256 - fy), w10 = fx * (256 - fy), w01 = (256 - fx) * fy, w11 = fx * fy;
            const uint32_t r = ((p00 >> 11) * w00 + (p10 >> 11) * w10 + (p01 >> 11) * w01 + (p11 >> 11) * w11) >> 16;
            const uint32_t g = (((p00 >> 5) & 0x3F) * w00 + ((p10 >> 5) & 0x3F) * w10 + ((p01 >> 5) & 0x3F) * w01 +
                                ((p11 >> 5) & 0x3F) * w11) >> 16;
            const uint32_t b = ((p00 & 0x1F) * w00 + (p10 & 0x1F) * w10 + (p01 & 0x1F) * w01 + (p11 & 0x1F) * w11) >> 16;
            row[x] = blend565(mode, row[x], r << 11 | g << 5 | b, k);
        }
    }
    set_box(box, bx0, by0, bx1, by1);
}

void od_transform(uint16_t *dst, int dw, int dh, const uint16_t *src, int sw, int sh, float px, float py, float sx,
                  float sy, float deg, float ax, float ay) {
    memset(dst, 0, (size_t)dw * dh * sizeof(uint16_t));
    od_transform_add(dst, dw, dh, src, sw, sh, px, py, sx, sy, deg, ax, ay, 256, OD_ADD, NULL);
}

/** For row y, the columns [s[0], s[1]] and [s[2], s[3]] (inclusive; first > second when none) that the ring between
 *  radii ri and ro about (cx, cy) touches, within 0 .. dw - 1. */
static void ring_row(int dw, float cx, float cy, float ri, float ro, int y, int s[4]) {
    s[0] = 1, s[1] = 0, s[2] = 1, s[3] = 0;
    const float dy = fabsf(y + 0.5f - cy);
    if (dy > ro) return;
    const float xo = sqrtf(ro * ro - dy * dy);
    int a0 = (int)floorf(cx - xo), b1 = (int)ceilf(cx + xo);
    if (ri > 0 && dy < ri) {
        const float xi = sqrtf(ri * ri - dy * dy);
        const int a1 = (int)ceilf(cx - xi), b0 = (int)floorf(cx + xi);
        if (a1 < b0) {
            s[0] = a0, s[1] = a1, s[2] = b0, s[3] = b1;
        } else {
            s[0] = a0, s[1] = b1;
        }
    } else {
        s[0] = a0, s[1] = b1;
    }
    for (int i = 0; i < 4; i += 2) {
        if (s[i] < 0) s[i] = 0;
        if (s[i + 1] > dw - 1) s[i + 1] = dw - 1;
    }
}

void od_ring(uint16_t *dst, int dw, int dh, float cx, float cy, float r, float width, uint32_t rgb, float opa,
             int box[4]) {
    set_box(box, 1, 1, 0, 0);
    if (opa <= 0.004f || r <= 0 || width <= 0) return;
    if (opa > 1) opa = 1;
    const float half = width * 0.5f, ri = r - half - 1, ro = r + half + 1;
    const int y0 = cy - ro < 0 ? 0 : (int)floorf(cy - ro), y1 = cy + ro > dh - 1 ? dh - 1 : (int)ceilf(cy + ro);
    const int32_t R = (int32_t)((((rgb >> 16) & 0xFF) * 31 + 127) / 255), G = (int32_t)((((rgb >> 8) & 0xFF) * 63 + 127) / 255),
                  B = (int32_t)(((rgb & 0xFF) * 31 + 127) / 255);
    int bx0 = dw, bx1 = -1, by0 = dh, by1 = -1;
    for (int y = y0; y <= y1; y++) {
        int s[4];
        ring_row(dw, cx, cy, ri, ro, y, s);
        const float dy = y + 0.5f - cy;
        uint16_t *row = &dst[y * dw];
        for (int p = 0; p < 4; p += 2) {
            for (int x = s[p]; x <= s[p + 1]; x++) {
                const float dx = x + 0.5f - cx, cover = half + 0.5f - fabsf(sqrtf(dx * dx + dy * dy) - r);
                if (cover <= 0) continue;
                const int32_t k = (int32_t)((cover > 1 ? 1 : cover) * opa * 256 + 0.5f);
                const int32_t d = row[x], dr = d >> 11, dg = (d >> 5) & 0x3F, db = d & 0x1F;
                row[x] = (uint16_t)((dr + (((R - dr) * k) >> 8)) << 11 | (dg + (((G - dg) * k) >> 8)) << 5 |
                                    (db + (((B - db) * k) >> 8)));
                if (x < bx0) bx0 = x;
                if (x > bx1) bx1 = x;
                if (y < by0) by0 = y;
                if (y > by1) by1 = y;
            }
        }
    }
    if (bx0 <= bx1) set_box(box, bx0, by0, bx1, by1);
}

void od_ring_restore(uint16_t *dst, int dw, int dh, const uint16_t *bg, int bw, int bh, int bx, int by, float cx,
                     float cy, float r, float width) {
    if (r <= 0 || width <= 0) return;
    const float half = width * 0.5f, ri = r - half - 1, ro = r + half + 1;
    const int y0 = cy - ro < 0 ? 0 : (int)floorf(cy - ro), y1 = cy + ro > dh - 1 ? dh - 1 : (int)ceilf(cy + ro);
    for (int y = y0; y <= y1; y++) {
        int s[4];
        ring_row(dw, cx, cy, ri, ro, y, s);
        for (int p = 0; p < 4; p += 2) {
            if (s[p] > s[p + 1]) continue;
            const int span[4] = {s[p], y, s[p + 1], y};
            od_restore(dst, dw, dh, bg, bw, bh, bx, by, span);
        }
    }
}
