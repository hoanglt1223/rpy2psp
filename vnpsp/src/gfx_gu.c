/* PSP renderer on the GE. Draws into the platform layer's display list between
 * plat_frame_begin and plat_frame_end. Textures are sampled from main RAM;
 * small ones (glyph pages, thumbnails) are also kept in a VRAM pool with LRU
 * eviction because the GE reads VRAM faster.
 *
 * VRAM map (2 MiB): the platform layer's two 8888 frame buffers and its depth
 * buffer, then our two 5650 transition targets, then the texture pool. The
 * platform draws into frame buffer 0 first and swaps every frame, so the
 * draw buffer alternates with the frame count. */
#include "gfx.h"

#include "base.h"

#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <stdlib.h>
#include <string.h>

#define FB_STRIDE 512
#define VRAM_TEX_MAX (64u * 1024u)
#define POOL_BLOCKS 64

struct gfx_tex {
    uint8_t fmt;
    bool swizzled;
    uint16_t w, rows;
    uint8_t tw, th;
    uint16_t bufw;
    const void *data;
    uint32_t size;
    int pool;                 /* pool block index, -1 when only in RAM */
    uint64_t last_used;
    bool target;
};

typedef struct pool_block {
    uint32_t off, size;
    gfx_tex *owner;
} pool_block;

typedef struct vert {
    float u, v;
    uint32_t color;
    float x, y, z;
} vert;

static uint32_t fb_rel[2];
static uint32_t target_rel[2];
static struct gfx_tex target_tex[2];
static uint32_t pool_start, pool_size;
static pool_block blocks[POOL_BLOCKS];
static int nblocks;
static uint64_t frame_no;
static uint32_t draw_rel, last_drawn_rel;
static bool have_frame;
static int clip_x, clip_y, clip_w = GFX_W, clip_h = GFX_H;

static void *vram_ptr(uint32_t rel)
{
    return (void *)((uintptr_t)sceGeEdramGetAddr() + rel);
}

bool gfx_init(void)
{
    /* Same order as the platform layer's gu_setup, so these match its buffers. */
    fb_rel[0] = 0;
    fb_rel[1] = FB_STRIDE * GFX_H * 4;
    target_rel[0] = (uint32_t)(uintptr_t)guGetStaticVramBuffer(FB_STRIDE, GFX_H, GU_PSM_5650);
    target_rel[1] = (uint32_t)(uintptr_t)guGetStaticVramBuffer(FB_STRIDE, GFX_H, GU_PSM_5650);
    uint32_t end = target_rel[1] + FB_STRIDE * GFX_H * 2;
    uint32_t total = (uint32_t)sceGeEdramGetSize();
    pool_start = end;
    pool_size = total > end ? total - end : 0;
    for (int i = 0; i < 2; i++) {
        target_tex[i].fmt = GFX_5650;
        target_tex[i].w = GFX_W;
        target_tex[i].rows = GFX_H;
        target_tex[i].tw = 9;
        target_tex[i].th = 9;
        target_tex[i].bufw = FB_STRIDE;
        target_tex[i].data = vram_ptr(target_rel[i]);
        target_tex[i].pool = -1;
        target_tex[i].target = true;
    }
    nblocks = 0;
    frame_no = 0;
    draw_rel = fb_rel[0];
    vn_log("gfx: VRAM pool %u bytes at 0x%06x", (unsigned)pool_size, (unsigned)pool_start);
    return true;
}

void gfx_shutdown(void)
{
}

/* ---- VRAM pool ------------------------------------------------------------------ */

static void pool_release(int i)
{
    blocks[i].owner->pool = -1;
    memmove(blocks + i, blocks + i + 1, sizeof(pool_block) * (size_t)(nblocks - i - 1));
    nblocks--;
    for (int k = i; k < nblocks; k++) blocks[k].owner->pool = k;
}

static bool pool_alloc(gfx_tex *t)
{
    uint32_t need = (t->size + 63u) & ~63u;
    if (need > pool_size || need > VRAM_TEX_MAX) return false;
    for (int attempt = 0; attempt <= POOL_BLOCKS; attempt++) {
        uint32_t at = pool_start;
        int slot = 0;
        bool found = false;
        for (int i = 0; i <= nblocks; i++) {
            uint32_t next = i < nblocks ? blocks[i].off : pool_start + pool_size;
            if (next - at >= need) {
                slot = i;
                found = true;
                break;
            }
            if (i < nblocks) at = blocks[i].off + blocks[i].size;
        }
        if (found && nblocks < POOL_BLOCKS) {
            memmove(blocks + slot + 1, blocks + slot, sizeof(pool_block) * (size_t)(nblocks - slot));
            blocks[slot].off = at;
            blocks[slot].size = need;
            blocks[slot].owner = t;
            nblocks++;
            for (int k = 0; k < nblocks; k++) blocks[k].owner->pool = k;
            /* Uncached VRAM view, so the GE sees the bytes without a cache flush. */
            memcpy((void *)((uintptr_t)vram_ptr(at) | 0x40000000u), t->data, t->size);
            sceGuTexFlush();
            return true;
        }
        /* Evict the least recently used texture that this frame has not drawn. */
        int victim = -1;
        for (int i = 0; i < nblocks; i++)
            if (blocks[i].owner->last_used < frame_no && (victim < 0 || blocks[i].owner->last_used < blocks[victim].owner->last_used))
                victim = i;
        if (victim < 0) return false;
        pool_release(victim);
    }
    return false;
}

/* ---- textures -------------------------------------------------------------------- */

gfx_tex *gfx_tex_create(const gfx_tex_desc *d)
{
    if (!d || !d->data || ((uintptr_t)d->data & 15)) return NULL;
    gfx_tex *t = vn_calloc(1, sizeof *t);
    t->fmt = d->fmt;
    t->swizzled = d->swizzled;
    t->w = d->width;
    t->rows = d->rows;
    t->tw = d->tw_log2;
    t->th = d->th_log2;
    t->bufw = d->bufw;
    t->data = d->data;
    t->size = (uint32_t)d->size;
    t->pool = -1;
    /* The GE reads main RAM directly, so the bytes must leave the CPU cache. */
    sceKernelDcacheWritebackRange(d->data, (SceSize)d->size);
    return t;
}

void gfx_tex_destroy(gfx_tex *t)
{
    if (!t || t->target) return;
    if (t->pool >= 0) pool_release(t->pool);
    free(t);
}

static void bind(gfx_tex *t, int flags, const uint32_t *clut)
{
    t->last_used = frame_no;
    if (t->pool < 0 && !t->target && t->size <= VRAM_TEX_MAX) pool_alloc(t);
    const void *ptr = t->pool >= 0 ? vram_ptr(blocks[t->pool].off) : t->data;
    if (t->fmt == GFX_T4 || t->fmt == GFX_T8) {
        int n = t->fmt == GFX_T4 ? 16 : 256;
        uint32_t *c = sceGuGetMemory(n * 4 + 16);
        c = (uint32_t *)(((uintptr_t)c + 15) & ~(uintptr_t)15);
        if (clut)
            memcpy(c, clut, (size_t)n * 4);
        else
            memset(c, 0, (size_t)n * 4);
        sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
        sceGuClutLoad(n / 8, c);
    }
    if (t->target) sceGuTexFlush();
    sceGuTexMode(t->fmt, 0, 0, t->swizzled ? 1 : 0);
    sceGuTexImage(0, 1 << t->tw, 1 << t->th, t->bufw, ptr);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    int filter = (flags & GFX_NEAREST) ? GU_NEAREST : GU_LINEAR;
    sceGuTexFilter(filter, filter);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
}

static void blend_mode(int flags)
{
    if (flags & GFX_BLEND_ADD)
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFF);
    else
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
}

void gfx_draw_quad(gfx_tex *t, const float xy[8], const float uv[8], uint32_t modulate, int flags, const uint32_t *clut)
{
    if (!t || (modulate >> 24) == 0) return;
    vert *v = sceGuGetMemory(4 * sizeof(vert));
    /* Triangle strip order: top-left, top-right, bottom-left, bottom-right. */
    static const int order[4] = {0, 1, 3, 2};
    for (int i = 0; i < 4; i++) {
        int k = order[i];
        v[i].u = uv[2 * k];
        v[i].v = uv[2 * k + 1];
        v[i].color = modulate;
        v[i].x = xy[2 * k];
        v[i].y = xy[2 * k + 1];
        v[i].z = 0;
    }
    sceGuEnable(GU_TEXTURE_2D);
    bind(t, flags, clut);
    blend_mode(flags);
    sceGuDrawArray(GU_TRIANGLE_STRIP, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 4, 0, v);
}

void gfx_fill_rect(float x, float y, float w, float h, uint32_t rgba)
{
    if ((rgba >> 24) == 0 || w <= 0 || h <= 0) return;
    vert *v = sceGuGetMemory(2 * sizeof(vert));
    v[0].u = v[0].v = v[1].u = v[1].v = 0;
    v[0].color = v[1].color = rgba;
    v[0].x = x;
    v[0].y = y;
    v[1].x = x + w;
    v[1].y = y + h;
    v[0].z = v[1].z = 0;
    sceGuDisable(GU_TEXTURE_2D);
    blend_mode(GFX_BLEND_NORMAL);
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
}

void gfx_set_clip(int x, int y, int w, int h)
{
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > GFX_W) w = GFX_W - x;
    if (y + h > GFX_H) h = GFX_H - y;
    if (w <= 0 || h <= 0) {
        x = y = 0;
        w = h = 1;
    }
    clip_x = x;
    clip_y = y;
    clip_w = w;
    clip_h = h;
    sceGuScissor(x, y, w, h);
}

void gfx_reset_clip(void)
{
    gfx_set_clip(0, 0, GFX_W, GFX_H);
}

/* ---- frames and targets ------------------------------------------------------------- */

void gfx_frame_begin(uint32_t clear_rgba)
{
    draw_rel = fb_rel[frame_no & 1];
    sceGuClearColor(clear_rgba | 0xFF000000u);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuEnable(GU_BLEND);
    gfx_reset_clip();
}

void gfx_frame_end(void)
{
    last_drawn_rel = draw_rel;
    have_frame = true;
    frame_no++;
}

void gfx_target_begin(int index, uint32_t clear_rgba)
{
    sceGuDrawBufferList(GU_PSM_5650, (void *)(uintptr_t)target_rel[index & 1], FB_STRIDE);
    gfx_reset_clip();
    sceGuClearColor(clear_rgba | 0xFF000000u);
    sceGuClear(GU_COLOR_BUFFER_BIT);
}

void gfx_target_end(void)
{
    sceGuDrawBufferList(GU_PSM_8888, (void *)(uintptr_t)draw_rel, FB_STRIDE);
    gfx_reset_clip();
}

gfx_tex *gfx_target_tex(int index)
{
    return &target_tex[index & 1];
}

bool gfx_capture_rgb(uint8_t *rgb)
{
    if (!have_frame) return false;
    const uint32_t *px = (const uint32_t *)((uintptr_t)vram_ptr(last_drawn_rel) | 0x40000000u);
    for (int y = 0; y < GFX_H; y++) {
        for (int x = 0; x < GFX_W; x++) {
            uint32_t c = px[y * FB_STRIDE + x];
            uint8_t *o = rgb + 3 * ((size_t)y * GFX_W + (size_t)x);
            o[0] = (uint8_t)c;
            o[1] = (uint8_t)(c >> 8);
            o[2] = (uint8_t)(c >> 16);
        }
    }
    return true;
}
