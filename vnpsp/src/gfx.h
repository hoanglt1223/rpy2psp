/* Renderer: textured quads and solid rectangles on a 480x272 frame.
 * Backends: gfx_soft.c (PC; deterministic, used headless and blitted through
 * the platform's GL window) and gfx_gu.c (PSP GE). Colours are 0xAABBGGRR,
 * the GE's 8888 order. Texture data stays owned by the caller (asset cache)
 * and must outlive the gfx_tex made from it. */
#ifndef VN_GFX_H
#define VN_GFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GFX_W 480
#define GFX_H 272

/* GE texture formats. */
enum { GFX_5650 = 0, GFX_8888 = 3, GFX_T4 = 4, GFX_T8 = 5, GFX_DXT1 = 8, GFX_DXT5 = 10 };

typedef struct gfx_tex gfx_tex;

typedef struct gfx_tex_desc {
    uint8_t fmt;
    bool swizzled;           /* T4/T8 glyph pages */
    uint16_t width, rows;    /* pixels stored (rows may be fewer than 2^th_log2) */
    uint8_t tw_log2, th_log2;
    uint16_t bufw;           /* pixels per stored row */
    const void *data;
    size_t size;
} gfx_tex_desc;

enum { GFX_BLEND_NORMAL = 0, GFX_BLEND_ADD = 1, GFX_NEAREST = 2 };

static inline uint32_t gfx_rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return (a << 24) | (b << 16) | (g << 8) | r;
}

bool gfx_init(void);
void gfx_shutdown(void);

gfx_tex *gfx_tex_create(const gfx_tex_desc *d);
void gfx_tex_destroy(gfx_tex *t);

/* Starts a frame cleared to clear_rgba. On PSP the platform's frame must already be open. */
void gfx_frame_begin(uint32_t clear_rgba);
void gfx_frame_end(void);

/* Quad corners in order top-left, top-right, bottom-right, bottom-left; u/v in
 * texels. The quad must be a parallelogram (any affine transform of a rectangle).
 * clut: 16 (T4) or 256 (T8) colours for indexed textures, NULL otherwise. */
void gfx_draw_quad(gfx_tex *t, const float xy[8], const float uv[8], uint32_t modulate, int flags,
                   const uint32_t *clut);
void gfx_fill_rect(float x, float y, float w, float h, uint32_t rgba);
void gfx_set_clip(int x, int y, int w, int h);
void gfx_reset_clip(void);

/* Off-screen 480x272 targets for transitions. Between begin and end every draw
 * lands in target `index` (0 or 1); gfx_target_tex returns it as a texture. */
void gfx_target_begin(int index, uint32_t clear_rgba);
void gfx_target_end(void);
gfx_tex *gfx_target_tex(int index);

/* Copies the last completed frame as 8-bit RGB, top row first. */
bool gfx_capture_rgb(uint8_t *rgb);

#endif
