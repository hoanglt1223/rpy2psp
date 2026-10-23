/* Software renderer with the GE's texture semantics: DXT decoded as the GE does
 * (RGB565 expanded by shifting, PPSSPP's block layout), swizzled T4/T8 with a
 * CLUT, bilinear filtering clamped to the stored texels, modulate and
 * src-alpha blending. Deterministic, so headless runs can hash frames. */
#include "gfx.h"
#include "gfx_soft.h"

#include "base.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct gfx_tex {
    uint8_t fmt;
    uint16_t w, h;        /* texels addressable (stored width and rows) */
    uint32_t *rgba;       /* direct colour formats, decoded */
    uint8_t *index;       /* T4/T8 indexes */
    bool borrowed;        /* target texture: pixels owned by the renderer */
};

static uint32_t frame[GFX_W * GFX_H];
static uint32_t shown[GFX_W * GFX_H];
static uint32_t target_px[2][GFX_W * GFX_H];
static struct gfx_tex target_tex[2];
static uint32_t *dst = frame;
static int clip_x0, clip_y0, clip_x1 = GFX_W, clip_y1 = GFX_H;
static bool have_shown;

bool gfx_init(void)
{
    for (int i = 0; i < 2; i++) {
        target_tex[i].fmt = GFX_8888;
        target_tex[i].w = GFX_W;
        target_tex[i].h = GFX_H;
        target_tex[i].rgba = target_px[i];
        target_tex[i].borrowed = true;
    }
    memset(shown, 0, sizeof shown);
    have_shown = false;
    return true;
}

void gfx_shutdown(void)
{
}

const uint32_t *gfx_soft_pixels(void)
{
    return shown;
}

/* ---- texture decoding ---------------------------------------------------------- */

static uint32_t col(int r, int g, int b, int a)
{
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

static void dxt_block(const uint8_t *b, bool dxt5, uint32_t out[16])
{
    uint16_t c1 = vn_le16(b + 4), c2 = vn_le16(b + 6);
    int r1 = (c1 >> 8) & 0xF8, g1 = (c1 >> 3) & 0xFC, b1 = (c1 << 3) & 0xF8;
    int r2 = (c2 >> 8) & 0xF8, g2 = (c2 >> 3) & 0xFC, b2 = (c2 << 3) & 0xF8;
    uint32_t pal[4];
    pal[0] = col(r1, g1, b1, 255);
    pal[1] = col(r2, g2, b2, 255);
    if (c1 > c2) {
        pal[2] = col((2 * r1 + r2) / 3, (2 * g1 + g2) / 3, (2 * b1 + b2) / 3, 255);
        pal[3] = col((2 * r2 + r1) / 3, (2 * g2 + g1) / 3, (2 * b2 + b1) / 3, 255);
    } else {
        pal[2] = col((r1 + r2) / 2, (g1 + g2) / 2, (b1 + b2) / 2, 255);
        pal[3] = col(0, 0, 0, dxt5 ? 255 : 0);
    }
    uint8_t apal[8];
    uint64_t abits = 0;
    if (dxt5) {
        int a1 = b[14], a2 = b[15];
        apal[0] = (uint8_t)a1;
        apal[1] = (uint8_t)a2;
        for (int n = 1; n < 7; n++) {
            if (a1 > a2)
                apal[n + 1] = (uint8_t)(((a1 * ((7 - n) << 8)) / 7 + (a2 * (n << 8)) / 7 + 31) >> 8);
            else if (n <= 4)
                apal[n + 1] = (uint8_t)(((a1 * ((5 - n) << 8)) / 5 + (a2 * (n << 8)) / 5 + 31) >> 8);
            else
                apal[n + 1] = n == 5 ? 0 : 255;
        }
        for (int k = 0; k < 6; k++) abits |= (uint64_t)b[8 + k] << (8 * k);
    }
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            uint32_t c = pal[(b[y] >> (2 * x)) & 3];
            if (dxt5) c = (c & 0x00FFFFFFu) | ((uint32_t)apal[(abits >> (12 * y + 3 * x)) & 7] << 24);
            out[y * 4 + x] = c;
        }
    }
}

gfx_tex *gfx_tex_create(const gfx_tex_desc *d)
{
    if (!d || !d->data || d->width == 0 || d->rows == 0 || d->bufw < d->width) return NULL;
    gfx_tex *t = vn_calloc(1, sizeof *t);
    t->fmt = d->fmt;
    t->w = d->width;
    t->h = d->rows;
    const uint8_t *src = d->data;
    if (d->fmt == GFX_DXT1 || d->fmt == GFX_DXT5) {
        bool dxt5 = d->fmt == GFX_DXT5;
        size_t bs = dxt5 ? 16 : 8, bw = d->bufw / 4u, bh = d->rows / 4u;
        if (d->size < bw * bh * bs || d->rows % 4) goto bad;
        t->rgba = vn_alloc(sizeof(uint32_t) * d->width * d->rows);
        uint32_t px[16];
        for (size_t by = 0; by < bh; by++) {
            for (size_t bx = 0; bx < bw; bx++) {
                dxt_block(src + (by * bw + bx) * bs, dxt5, px);
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        size_t X = bx * 4 + (size_t)x, Y = by * 4 + (size_t)y;
                        if (X < d->width) t->rgba[Y * d->width + X] = px[y * 4 + x];
                    }
            }
        }
    } else if (d->fmt == GFX_T4 || d->fmt == GFX_T8) {
        size_t wb = d->fmt == GFX_T4 ? d->bufw / 2u : d->bufw;
        if (d->size < wb * d->rows || (d->swizzled && (wb % 16 || d->rows % 8))) goto bad;
        t->index = vn_alloc((size_t)d->width * d->rows);
        for (size_t y = 0; y < d->rows; y++) {
            for (size_t xb = 0; xb < wb; xb++) {
                size_t off = d->swizzled ? ((y / 8) * (wb / 16) + xb / 16) * 128 + (y % 8) * 16 + xb % 16 : y * wb + xb;
                uint8_t byte = src[off];
                if (d->fmt == GFX_T4) {
                    size_t x = xb * 2;
                    if (x < d->width) t->index[y * d->width + x] = byte & 15;
                    if (x + 1 < d->width) t->index[y * d->width + x + 1] = byte >> 4;
                } else if (xb < d->width) {
                    t->index[y * d->width + xb] = byte;
                }
            }
        }
    } else if (d->fmt == GFX_8888) {
        if (d->size < (size_t)d->bufw * d->rows * 4) goto bad;
        t->rgba = vn_alloc(sizeof(uint32_t) * d->width * d->rows);
        for (size_t y = 0; y < d->rows; y++) memcpy(t->rgba + y * d->width, src + y * d->bufw * 4, (size_t)d->width * 4);
    } else {
        goto bad;
    }
    return t;
bad:
    free(t);
    return NULL;
}

void gfx_tex_destroy(gfx_tex *t)
{
    if (!t || t->borrowed) return;
    free(t->rgba);
    free(t->index);
    free(t);
}

/* ---- frame and targets --------------------------------------------------------- */

static void clear(uint32_t *buf, uint32_t c)
{
    for (int i = 0; i < GFX_W * GFX_H; i++) buf[i] = c;
}

void gfx_frame_begin(uint32_t clear_rgba)
{
    dst = frame;
    gfx_reset_clip();
    clear(frame, clear_rgba | 0xFF000000u);
}

void gfx_frame_end(void)
{
    memcpy(shown, frame, sizeof shown);
    have_shown = true;
}

void gfx_target_begin(int index, uint32_t clear_rgba)
{
    dst = target_px[index & 1];
    gfx_reset_clip();
    clear(dst, clear_rgba | 0xFF000000u);
}

void gfx_target_end(void)
{
    dst = frame;
    gfx_reset_clip();
}

gfx_tex *gfx_target_tex(int index)
{
    return &target_tex[index & 1];
}

bool gfx_capture_rgb(uint8_t *rgb)
{
    if (!have_shown) return false;
    for (int i = 0; i < GFX_W * GFX_H; i++) {
        rgb[3 * i] = (uint8_t)shown[i];
        rgb[3 * i + 1] = (uint8_t)(shown[i] >> 8);
        rgb[3 * i + 2] = (uint8_t)(shown[i] >> 16);
    }
    return true;
}

void gfx_set_clip(int x, int y, int w, int h)
{
    clip_x0 = x < 0 ? 0 : x;
    clip_y0 = y < 0 ? 0 : y;
    clip_x1 = x + w > GFX_W ? GFX_W : x + w;
    clip_y1 = y + h > GFX_H ? GFX_H : y + h;
}

void gfx_reset_clip(void)
{
    clip_x0 = clip_y0 = 0;
    clip_x1 = GFX_W;
    clip_y1 = GFX_H;
}

/* ---- drawing ------------------------------------------------------------------ */

static inline uint32_t mul8(uint32_t a, uint32_t b)
{
    return (a * b + 127) / 255;
}

static inline void blend(uint32_t *d, uint32_t s, int flags)
{
    uint32_t a = s >> 24;
    if (a == 0) return;
    uint32_t dv = *d;
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        uint32_t sc = (s >> sh) & 0xFF, dc = (dv >> sh) & 0xFF, r;
        if (flags & GFX_BLEND_ADD) {
            r = dc + mul8(sc, a);
            if (r > 255) r = 255;
        } else {
            r = (sc * a + dc * (255 - a) + 127) / 255;
        }
        out |= r << sh;
    }
    *d = out | 0xFF000000u;
}

static inline uint32_t fetch(const gfx_tex *t, int x, int y, const uint32_t *clut)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= t->w) x = t->w - 1;
    if (y >= t->h) y = t->h - 1;
    if (t->index) return clut ? clut[t->index[y * t->w + x] & (t->fmt == GFX_T4 ? 15 : 255)] : 0;
    return t->rgba[y * t->w + x];
}

static inline uint32_t lerp_px(uint32_t a, uint32_t b, uint32_t f)   /* f in 0..256 */
{
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        uint32_t x = (a >> sh) & 0xFF, y = (b >> sh) & 0xFF;
        out |= ((x * (256 - f) + y * f + 128) >> 8) << sh;
    }
    return out;
}

static uint32_t sample(const gfx_tex *t, float u, float v, int flags, const uint32_t *clut)
{
    if (flags & GFX_NEAREST) return fetch(t, (int)floorf(u), (int)floorf(v), clut);
    float fu = u - 0.5f, fv = v - 0.5f;
    int x = (int)floorf(fu), y = (int)floorf(fv);
    uint32_t ax = (uint32_t)((fu - (float)x) * 256.0f + 0.5f), ay = (uint32_t)((fv - (float)y) * 256.0f + 0.5f);
    if (ax == 0 && ay == 0) return fetch(t, x, y, clut);
    uint32_t top = lerp_px(fetch(t, x, y, clut), fetch(t, x + 1, y, clut), ax);
    uint32_t bot = lerp_px(fetch(t, x, y + 1, clut), fetch(t, x + 1, y + 1, clut), ax);
    return lerp_px(top, bot, ay);
}

static inline uint32_t modulate(uint32_t c, uint32_t m)
{
    if (m == 0xFFFFFFFFu) return c;
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) out |= mul8((c >> sh) & 0xFF, (m >> sh) & 0xFF) << sh;
    return out;
}

void gfx_draw_quad(gfx_tex *t, const float xy[8], const float uv[8], uint32_t mod, int flags, const uint32_t *clut)
{
    if (!t || (mod >> 24) == 0) return;
    float e1x = xy[2] - xy[0], e1y = xy[3] - xy[1], e2x = xy[6] - xy[0], e2y = xy[7] - xy[1];
    float det = e1x * e2y - e1y * e2x;
    if (fabsf(det) < 1e-6f) return;
    float minx = xy[0], maxx = xy[0], miny = xy[1], maxy = xy[1];
    for (int i = 1; i < 4; i++) {
        minx = fminf(minx, xy[2 * i]);
        maxx = fmaxf(maxx, xy[2 * i]);
        miny = fminf(miny, xy[2 * i + 1]);
        maxy = fmaxf(maxy, xy[2 * i + 1]);
    }
    int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx), y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    if (x0 < clip_x0) x0 = clip_x0;
    if (y0 < clip_y0) y0 = clip_y0;
    if (x1 > clip_x1) x1 = clip_x1;
    if (y1 > clip_y1) y1 = clip_y1;
    if (x0 >= x1 || y0 >= y1) return;

    float du_s = uv[2] - uv[0], dv_s = uv[3] - uv[1], du_t = uv[6] - uv[0], dv_t = uv[7] - uv[1];
    bool axis = e1y == 0.0f && e2x == 0.0f && dv_s == 0.0f && du_t == 0.0f;
    /* Exact 1:1 copies (the common case: images are pre-scaled to screen pixels). */
    if (axis && e1x == du_s && e2y == dv_t && e1x > 0 && e2y > 0 && xy[0] == floorf(xy[0]) &&
        xy[1] == floorf(xy[1]) && uv[0] == floorf(uv[0]) && uv[1] == floorf(uv[1])) {
        int ou = (int)uv[0] - (int)xy[0], ov = (int)uv[1] - (int)xy[1];
        for (int y = y0; y < y1; y++) {
            uint32_t *row = dst + y * GFX_W;
            for (int x = x0; x < x1; x++) blend(&row[x], modulate(fetch(t, x + ou, y + ov, clut), mod), flags);
        }
        return;
    }
    for (int y = y0; y < y1; y++) {
        uint32_t *row = dst + y * GFX_W;
        float py = (float)y + 0.5f - xy[1];
        for (int x = x0; x < x1; x++) {
            float px = (float)x + 0.5f - xy[0];
            float s = (px * e2y - py * e2x) / det, tt = (e1x * py - e1y * px) / det;
            if (s < 0.0f || s >= 1.0f || tt < 0.0f || tt >= 1.0f) continue;
            float u = uv[0] + s * du_s + tt * du_t, v = uv[1] + s * dv_s + tt * dv_t;
            blend(&row[x], modulate(sample(t, u, v, flags, clut), mod), flags);
        }
    }
}

void gfx_fill_rect(float x, float y, float w, float h, uint32_t rgba)
{
    if ((rgba >> 24) == 0 || w <= 0 || h <= 0) return;
    int x0 = (int)floorf(x + 0.5f), y0 = (int)floorf(y + 0.5f), x1 = (int)floorf(x + w + 0.5f),
        y1 = (int)floorf(y + h + 0.5f);
    if (x0 < clip_x0) x0 = clip_x0;
    if (y0 < clip_y0) y0 = clip_y0;
    if (x1 > clip_x1) x1 = clip_x1;
    if (y1 > clip_y1) y1 = clip_y1;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) blend(&dst[yy * GFX_W + xx], rgba, GFX_BLEND_NORMAL);
}
