/* `with` transitions from the last committed screen to the current scene.
 * The old screen is rendered once into off-screen target 0 and composited
 * with the live new scene; move transitions interpolate sprite positions. */
#include "engine_internal.h"

#include <math.h>

float trans_duration(const vn_trans *t)
{
    switch (t->kind) {
    case TR_FADE: return t->params[0] + t->params[1] + t->params[2];
    case TR_SHAKE: return t->params[5];
    default: return t->params[0];
    }
}

static bool needs_snapshot(const vn_trans *t)
{
    return t->kind == TR_DISSOLVE || t->kind == TR_FADE || t->kind == TR_PIXELLATE || t->kind == TR_CROPMOVE ||
           t->kind == TR_PUSHMOVE;
}

void trans_start(vn_engine *e, const vn_trans *t)
{
    e->trans = t;
    e->trans_t = 0;
    e->trans_len = trans_duration(t);
    e->snapshot_pending = needs_snapshot(t);
}

bool trans_update(vn_engine *e, float dt)
{
    e->trans_t += dt;
    return !e->trans || e->trans_t >= e->trans_len;
}

void trans_finish(vn_engine *e)
{
    e->trans = NULL;
    e->snapshot_pending = false;
    scene_copy(&e->committed, &e->st.sc);
}

static void draw_target(int index, float x, float y, float alpha, float cx0, float cy0, float cx1, float cy1)
{
    /* Draws the part [cx0, cx1) x [cy0, cy1) of a full-screen target, shifted by (x, y). */
    float xy[8] = {cx0 + x, cy0 + y, cx1 + x, cy0 + y, cx1 + x, cy1 + y, cx0 + x, cy1 + y};
    float uv[8] = {cx0, cy0, cx1, cy0, cx1, cy1, cx0, cy1};
    gfx_draw_quad(gfx_target_tex(index), xy, uv, gfx_rgba(255, 255, 255, (uint32_t)(vn_clampf(alpha, 0, 1) * 255.0f + 0.5f)),
                  GFX_NEAREST, NULL);
}

static void draw_full_target(int index, float x, float y, float alpha)
{
    draw_target(index, x, y, alpha, 0, 0, GFX_W, GFX_H);
}

/* Downscales target `src` by `block` into the corner of target `tmp`, then draws it back up. */
static void pixellate(int src, int tmp, float block)
{
    int w = (int)ceilf(GFX_W / block), h = (int)ceilf(GFX_H / block);
    gfx_target_begin(tmp, 0);
    float xy[8] = {0, 0, (float)w, 0, (float)w, (float)h, 0, (float)h};
    float uv[8] = {0, 0, GFX_W, 0, GFX_W, GFX_H, 0, GFX_H};
    gfx_draw_quad(gfx_target_tex(src), xy, uv, 0xFFFFFFFFu, GFX_NEAREST, NULL);
    gfx_target_end();
    float up[8] = {0, 0, w * block, 0, w * block, h * block, 0, h * block};
    float uv2[8] = {0, 0, (float)w, 0, (float)w, (float)h, 0, (float)h};
    gfx_draw_quad(gfx_target_tex(tmp), up, uv2, 0xFFFFFFFFu, GFX_NEAREST, NULL);
}

void trans_draw(vn_engine *e)
{
    const vn_trans *t = e->trans;
    scene_ctx *c = &e->sctx;
    float len = e->trans_len > 0 ? e->trans_len : 1e-6f;
    float f = vn_clampf(e->trans_t / len, 0.0f, 1.0f);
    float W = GFX_W, H = GFX_H;
    switch (t->kind) {
    case TR_DISSOLVE:
        scene_draw(c, &e->st.sc, 0, 0, NULL);
        draw_full_target(0, 0, 0, 1.0f - atl_warp(t->warper, f));
        break;
    case TR_FADE: {
        float out = t->params[0], hold = t->params[1], in = t->params[2], tt = e->trans_t;
        uint32_t rgb = gfx_rgba(t->color[0], t->color[1], t->color[2], 0);
        if (tt < out) {
            draw_full_target(0, 0, 0, 1.0f);
            gfx_fill_rect(0, 0, W, H, rgb | ((uint32_t)(255.0f * tt / out) << 24));
        } else if (tt < out + hold) {
            gfx_fill_rect(0, 0, W, H, rgb | 0xFF000000u);
        } else {
            scene_draw(c, &e->st.sc, 0, 0, NULL);
            float a = in > 0 ? 1.0f - (tt - out - hold) / in : 0.0f;
            gfx_fill_rect(0, 0, W, H, rgb | ((uint32_t)(255.0f * vn_clampf(a, 0, 1)) << 24));
        }
        break;
    }
    case TR_PIXELLATE: {
        float steps = t->params[1] > 0 ? t->params[1] : 5.0f;
        if (f < 0.5f) {
            float block = powf(2.0f, ceilf(steps * f * 2.0f));
            pixellate(0, 1, block);
        } else {
            gfx_target_begin(1, 0);
            scene_draw(c, &e->st.sc, 0, 0, NULL);
            gfx_target_end();
            float block = powf(2.0f, ceilf(steps * (1.0f - f) * 2.0f));
            pixellate(1, 0, block);
        }
        break;
    }
    case TR_MOVE: {
        move_anim m = {&e->committed, atl_warp(t->warper, f), t->enter, t->leave, atl_warp(t->enter_warper, f),
                       atl_warp(t->leave_warper, f)};
        scene_draw(c, &e->st.sc, 0, 0, &m);
        break;
    }
    case TR_CROPMOVE: {
        int mode = (int)t->params[1];
        switch (mode) {
        case 0: scene_draw(c, &e->st.sc, 0, 0, NULL); draw_target(0, 0, 0, 1, f * W, 0, W, H); break;
        case 1: scene_draw(c, &e->st.sc, 0, 0, NULL); draw_target(0, 0, 0, 1, 0, 0, (1 - f) * W, H); break;
        case 2: scene_draw(c, &e->st.sc, 0, 0, NULL); draw_target(0, 0, 0, 1, 0, 0, W, (1 - f) * H); break;
        case 3: scene_draw(c, &e->st.sc, 0, 0, NULL); draw_target(0, 0, 0, 1, 0, f * H, W, H); break;
        case 4: draw_full_target(0, 0, 0, 1); scene_draw(c, &e->st.sc, (f - 1) * W, 0, NULL); break;
        case 5: draw_full_target(0, 0, 0, 1); scene_draw(c, &e->st.sc, (1 - f) * W, 0, NULL); break;
        case 6: draw_full_target(0, 0, 0, 1); scene_draw(c, &e->st.sc, 0, (1 - f) * H, NULL); break;
        case 7: draw_full_target(0, 0, 0, 1); scene_draw(c, &e->st.sc, 0, (f - 1) * H, NULL); break;
        case 8: scene_draw(c, &e->st.sc, 0, 0, NULL); draw_full_target(0, f * W, 0, 1); break;
        case 9: scene_draw(c, &e->st.sc, 0, 0, NULL); draw_full_target(0, -f * W, 0, 1); break;
        case 10: scene_draw(c, &e->st.sc, 0, 0, NULL); draw_full_target(0, 0, -f * H, 1); break;
        default: scene_draw(c, &e->st.sc, 0, 0, NULL); draw_full_target(0, 0, f * H, 1); break;
        }
        break;
    }
    case TR_PUSHMOVE: {
        int mode = (int)t->params[1];
        float ox = 0, oy = 0, nx = 0, ny = 0;
        if (mode == 0) { ox = f * W; nx = (f - 1) * W; }
        else if (mode == 1) { ox = -f * W; nx = (1 - f) * W; }
        else if (mode == 2) { oy = -f * H; ny = (1 - f) * H; }
        else { oy = f * H; ny = (f - 1) * H; }
        draw_full_target(0, ox, oy, 1);
        scene_draw(c, &e->st.sc, nx, ny, NULL);
        break;
    }
    case TR_SHAKE: {
        float period = t->params[4] > 0 ? t->params[4] : 0.1f;
        float ph = fmodf(e->trans_t, period) / period;
        bool bounce = t->warper & 1, repeat = (t->warper & 2) != 0;
        if (!repeat && e->trans_t >= period) ph = 1.0f;
        if (bounce) ph = ph < 0.5f ? ph * 2.0f : 2.0f - ph * 2.0f;
        float dx = (t->params[0] + (t->params[2] - t->params[0]) * ph) * c->v.k;
        float dy = (t->params[1] + (t->params[3] - t->params[1]) * ph) * c->v.k;
        scene_draw(c, &e->st.sc, floorf(dx + 0.5f), floorf(dy + 0.5f), NULL);
        break;
    }
    default:
        scene_draw(c, &e->st.sc, 0, 0, NULL);
        break;
    }
}
