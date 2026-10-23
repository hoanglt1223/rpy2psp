#include "scene.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DEG2RAD 0.017453292519943295f

void scene_init(scene *sc)
{
    memset(sc, 0, sizeof *sc);
}

void scene_free(scene *sc)
{
    free(sc->items);
    memset(sc, 0, sizeof *sc);
}

void scene_clear(scene *sc)
{
    sc->n = 0;
}

void scene_copy(scene *dst, const scene *src)
{
    if (dst->cap < src->n) {
        dst->cap = src->n;
        dst->items = vn_realloc(dst->items, sizeof(sprite) * dst->cap);
    }
    if (src->n) memcpy(dst->items, src->items, sizeof(sprite) * src->n);
    dst->n = src->n;
}

sprite *scene_find(scene *sc, const char *key)
{
    for (uint32_t i = 0; i < sc->n; i++)
        if (strcmp(sc->items[i].key, key) == 0) return &sc->items[i];
    return NULL;
}

view scene_view(const vn_script *s)
{
    view v;
    v.k = fminf(GFX_W / s->src_w, GFX_H / s->src_h);
    v.ox = floorf((GFX_W - s->src_w * v.k) / 2.0f + 0.5f);
    v.oy = floorf((GFX_H - s->src_h * v.k) / 2.0f + 0.5f);
    return v;
}

/* ---- attributes --------------------------------------------------------------- */

static bool in_list(const char *a, const char *const *list, int n)
{
    for (int i = 0; i < n; i++)
        if (strcmp(a, list[i]) == 0) return true;
    return false;
}

uint16_t scene_resolve(const vn_script *s, const char *tag, const char *const *attrs, int nattrs, uint16_t current)
{
    const char *req[32], *removed[32];
    int nreq = 0, nrem = 0;
    for (int i = 0; i < nattrs && i < 32; i++) {
        if (attrs[i][0] == '-')
            removed[nrem++] = attrs[i] + 1;
        else
            req[nreq++] = attrs[i];
    }
    const vn_image *cur = current < s->nimages && strcmp(s->images[current].tag, tag) == 0 ? &s->images[current] : NULL;
    uint16_t best = VN_NONE16;
    int best_score = -1;
    for (uint32_t i = 0; i < s->nimages; i++) {
        const vn_image *im = &s->images[i];
        if (strcmp(im->tag, tag) != 0) continue;
        bool ok = true;
        for (int k = 0; ok && k < nreq; k++) ok = in_list(req[k], im->attrs, im->nattrs);
        int score = 0;
        for (int k = 0; ok && k < im->nattrs; k++) {
            const char *a = im->attrs[k];
            if (in_list(a, req, nreq)) continue;
            /* Every other attribute must be a current one that was not removed. */
            if (cur && in_list(a, cur->attrs, cur->nattrs) && !in_list(a, removed, nrem))
                score++;
            else
                ok = false;
        }
        if (ok && score > best_score) {
            best_score = score;
            best = (uint16_t)i;
        }
    }
    return best;
}

/* ---- show / hide ----------------------------------------------------------------- */

static uint32_t insert_index(scene *sc, int32_t zorder, const vn_script *s, const vn_show *rec)
{
    if (rec && rec->nbehind) {
        for (uint32_t i = 0; i < sc->n; i++)
            for (int k = 0; k < rec->nbehind; k++)
                if (strcmp(sc->items[i].key, vn_str(s, vn_le32(rec->behind + 4 * k))) == 0) return i;
    }
    uint32_t idx = 0;
    for (uint32_t i = 0; i < sc->n; i++)
        if (sc->items[i].zorder <= zorder) idx = i + 1;
    return idx;
}

static void insert_at(scene *sc, uint32_t idx, const sprite *sp)
{
    if (sc->n == sc->cap) {
        sc->cap = sc->cap ? sc->cap * 2 : 8;
        sc->items = vn_realloc(sc->items, sizeof(sprite) * sc->cap);
    }
    memmove(sc->items + idx + 1, sc->items + idx, sizeof(sprite) * (sc->n - idx));
    sc->items[idx] = *sp;
    sc->n++;
}

static void remove_at(scene *sc, uint32_t idx)
{
    memmove(sc->items + idx, sc->items + idx + 1, sizeof(sprite) * (sc->n - idx - 1));
    sc->n--;
}

static void copy_key(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n >= SCENE_KEY_MAX) n = SCENE_KEY_MAX - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void scene_show(scene_ctx *c, scene *sc, const vn_show *rec, const char *tag, const char *const *attrs, int nattrs)
{
    const vn_script *s = c->s;
    const char *rec_attrs[64];
    if (!tag) {
        tag = vn_str(s, rec->tag);
        nattrs = rec->nattrs;
        for (int i = 0; i < nattrs; i++) rec_attrs[i] = vn_str(s, vn_le32(rec->attrs + 4 * i));
        attrs = rec_attrs;
    }
    const char *key = rec->as != VN_NONE32 ? vn_str(s, rec->as) : tag;
    sprite *old = scene_find(sc, key);
    uint16_t image = VN_NONE16, disp = rec->disp;
    if (disp == VN_NONE16) {
        image = scene_resolve(s, tag, attrs, nattrs, old ? old->image : VN_NONE16);
        if (image == VN_NONE16) return;   /* Ren'Py shows nothing for an unknown image */
        disp = s->images[image].disp;
        if (disp == VN_NONE16) return;
    }
    sprite sp;
    bool restart = rec->nat > 0 || rec->atl != VN_NONE16;
    if (old) {
        sp = *old;
        remove_at(sc, (uint32_t)(old - sc->items));
    } else {
        memset(&sp, 0, sizeof sp);
        copy_key(sp.key, key);
        sp.atl = VN_NONE16;
        restart = true;
    }
    sp.image = image;
    sp.disp = disp;
    if (rec->nat) {
        sp.nat = rec->nat > SCENE_AT_MAX ? SCENE_AT_MAX : rec->nat;
        for (int i = 0; i < sp.nat; i++) sp.at[i] = vn_le16(rec->at + 2 * i);
        sp.atl = VN_NONE16;
    }
    if (rec->atl != VN_NONE16) sp.atl = rec->atl;
    if (rec->has_zorder) sp.zorder = rec->zorder;
    if (restart) sp.shown_at = c->clock;
    uint32_t idx;
    if (old && !rec->has_zorder && !rec->nbehind)
        idx = (uint32_t)(old - sc->items);   /* keeps its place in the stacking order */
    else
        idx = insert_index(sc, sp.zorder, s, rec);
    if (idx > sc->n) idx = sc->n;
    insert_at(sc, idx, &sp);
}

void scene_hide(scene *sc, const char *key)
{
    sprite *sp = scene_find(sc, key);
    if (sp) remove_at(sc, (uint32_t)(sp - sc->items));
}

bool scene_set_attrs(scene_ctx *c, scene *sc, const char *tag, const char *const *attrs, int nattrs)
{
    sprite *sp = scene_find(sc, tag);
    if (!sp) return false;
    uint16_t image = scene_resolve(c->s, tag, attrs, nattrs, sp->image);
    if (image == VN_NONE16 || c->s->images[image].disp == VN_NONE16) return false;
    sp->image = image;
    sp->disp = c->s->images[image].disp;
    return true;
}

bool scene_showing(const vn_script *s, scene *sc, const char *name)
{
    char buf[256];
    size_t n = strlen(name);
    if (n >= sizeof buf) return false;
    memcpy(buf, name, n + 1);
    char *words[32];
    int nw = 0;
    for (char *p = strtok(buf, " "); p && nw < 32; p = strtok(NULL, " ")) words[nw++] = p;
    if (!nw) return false;
    sprite *sp = scene_find(sc, words[0]);
    if (!sp) return false;
    if (nw == 1) return true;
    if (sp->image >= s->nimages) return false;
    const vn_image *im = &s->images[sp->image];
    for (int i = 1; i < nw; i++)
        if (!in_list(words[i], im->attrs, im->nattrs)) return false;
    return true;
}

/* ---- layout --------------------------------------------------------------------- */

static void child_size(scene_ctx *c, uint16_t disp, float *w, float *h)
{
    const vn_disp *d = &c->s->disps[disp];
    *w = d->width;
    *h = d->height;
    switch (d->kind) {
    case DISP_SOLID:
        if (*w <= 0) *w = c->s->src_w;
        if (*h <= 0) *h = c->s->src_h;
        break;
    case DISP_TEXTURE: case DISP_FRAME:
        if ((*w <= 0 || *h <= 0) && d->asset != VN_NONE16) {
            vn_texture *t = vn_assets_texture(c->assets, d->asset);
            if (t) {
                *w = t->src_w;
                *h = t->src_h;
            }
        }
        break;
    case DISP_TEXT:
        if (c->measure_text) {
            c->measure_text(c->user, disp, w, h);
            *w /= c->v.k;
            *h /= c->v.k;
        }
        break;
    default: break;
    }
}

static void layout_with(scene_ctx *c, const sprite *sp, uint16_t extra, layout *out, const char *event)
{
    const vn_script *s = c->s;
    memset(out, 0, sizeof *out);
    if (sp->disp >= s->ndisps) return;
    double t = c->clock - sp->shown_at;
    atl_props p;
    atl_props_init(&p);
    atl_val half = {0.5f, AV_FLOAT}, one = {1.0f, AV_FLOAT};
    p.v[P_XPOS] = half;
    p.v[P_XANCHOR] = half;
    p.v[P_YPOS] = one;
    p.v[P_YANCHOR] = one;
    p.set = (1u << P_XPOS) | (1u << P_XANCHOR) | (1u << P_YPOS) | (1u << P_YANCHOR);

    atl_env env = {s->src_w, s->src_h, 0, 0, 0, event ? event : "show"};
    for (const char *k = sp->key; *k; k++) env.seed = env.seed * 31u + (uint8_t)*k;

    uint16_t leaf = sp->disp;
    for (int depth = 0; depth < 8; depth++) {
        const vn_disp *d = &s->disps[leaf];
        if (d->kind == DISP_TRANSFORMED && d->child < s->ndisps) {
            child_size(c, d->child, &env.child_w, &env.child_h);
            if (d->transform != VN_NONE16) atl_apply(s, d->transform, t, &p, &env);
            leaf = d->child;
        } else if (d->kind == DISP_ATL) {
            p.child = VN_NONE16;
            atl_apply(s, d->transform, t, &p, &env);
            if (p.child >= s->ndisps) return;
            leaf = p.child;
        } else {
            break;
        }
    }
    float cw, ch;
    child_size(c, leaf, &cw, &ch);
    env.child_w = cw;
    env.child_h = ch;
    for (int i = 0; i < sp->nat; i++) atl_apply(s, sp->at[i], t, &p, &env);
    if (sp->atl != VN_NONE16) atl_apply(s, sp->atl, t, &p, &env);
    if (extra != VN_NONE16) atl_apply(s, extra, 1e6, &p, &env);

#define GET(slot, dflt) (((p.set >> (slot)) & 1) ? p.v[slot] : (dflt))
    atl_val zero = {0, AV_FLOAT}, none = {0, AV_NONE}, zpx = {0, AV_PX};
    float cx = 0, cy = 0, cwid = cw, chei = ch;
    if ((p.set >> P_CROP) & 1) {
        cx = atl_px(p.v[P_CROP], cw);
        cy = atl_px(GET(P_CROP_Y, zero), ch);
        cwid = atl_px(GET(P_CROP_W, one), cw);
        chei = atl_px(GET(P_CROP_H, one), ch);
        if (cx < 0) cx = 0;
        if (cy < 0) cy = 0;
        if (cx + cwid > cw) cwid = cw - cx;
        if (cy + chei > ch) chei = ch - cy;
    }
    if (cwid <= 0 || chei <= 0) return;
    float sx = GET(P_ZOOM, one).v * GET(P_XZOOM, one).v, sy = GET(P_ZOOM, one).v * GET(P_YZOOM, one).v;
    atl_val xs = GET(P_XSIZE, none), ys = GET(P_YSIZE, none);
    if (xs.kind != AV_NONE) sx *= atl_px(xs, s->src_w) / cwid;
    if (ys.kind != AV_NONE) sy *= atl_px(ys, s->src_h) / chei;
    float w = cwid * sx, h = chei * sy;
    atl_val rotv = GET(P_ROTATE, none);
    float rot = rotv.kind == AV_NONE ? 0.0f : rotv.v * DEG2RAD;
    float cs = cosf(rot), sn = sinf(rot);
    float W = fabsf(w * cs) + fabsf(h * sn), H = fabsf(w * sn) + fabsf(h * cs);
    float x = atl_px(GET(P_XPOS, zero), s->src_w) - atl_px(GET(P_XANCHOR, zero), W) + atl_px(GET(P_XOFFSET, zpx), s->src_w);
    float y = atl_px(GET(P_YPOS, zero), s->src_h) - atl_px(GET(P_YANCHOR, zero), H) + atl_px(GET(P_YOFFSET, zpx), s->src_h);
    float ccx = x + W / 2, ccy = y + H / 2;
    float k = c->v.k;
    out->visible = true;
    out->leaf = leaf;
    out->child_w = cw;
    out->child_h = ch;
    out->crop[0] = cx;
    out->crop[1] = cy;
    out->crop[2] = cwid;
    out->crop[3] = chei;
    out->m[0] = k * cs * sx;
    out->m[1] = -k * sn * sy;
    out->m[2] = k * sn * sx;
    out->m[3] = k * cs * sy;
    out->tx = k * (ccx - (cs * w / 2 - sn * h / 2)) + c->v.ox;
    out->ty = k * (ccy - (sn * w / 2 + cs * h / 2)) + c->v.oy;
    out->alpha = vn_clampf(GET(P_ALPHA, one).v, 0.0f, 1.0f);
    out->additive = GET(P_ADDITIVE, zero).v > 0.0f;
    out->native = rot == 0.0f && fabsf(sx - 1.0f) < 1e-4f && fabsf(sy - 1.0f) < 1e-4f;
    if (out->native) {
        out->tx = floorf(out->tx + 0.5f);
        out->ty = floorf(out->ty + 0.5f);
    }
#undef GET
}

void scene_layout(scene_ctx *c, const sprite *sp, layout *out, const char *event)
{
    layout_with(c, sp, VN_NONE16, out, event);
}

/* ---- drawing ---------------------------------------------------------------- */

static void map_pt(const float m[4], float tx, float ty, float x, float y, float *ox, float *oy)
{
    *ox = tx + m[0] * x + m[1] * y;
    *oy = ty + m[2] * x + m[3] * y;
}

/* Draws texture source rect (texture source pixels) into child rect (child
 * pixels, before cropping), clipped to the layout's crop. */
static void draw_region(vn_texture *t, float sx, float sy, float sw, float sh, float x, float y, float w, float h,
                        const layout *l, float dx, float dy, uint32_t mod)
{
    if (sw <= 0 || sh <= 0 || w <= 0 || h <= 0) return;
    float ktx = t->src_w ? (float)t->w / t->src_w : 1.0f, kty = t->src_h ? (float)t->h / t->src_h : 1.0f;
    float m[4] = {l->m[0], l->m[1], l->m[2], l->m[3]};
    if (l->native) {
        m[0] = ktx;
        m[3] = kty;
        m[1] = m[2] = 0;
    }
    float tx = l->tx + dx, ty = l->ty + dy;
    /* Clip the destination to the crop; shrink the source to match. */
    float x0 = fmaxf(x, l->crop[0]), y0 = fmaxf(y, l->crop[1]);
    float x1 = fminf(x + w, l->crop[0] + l->crop[2]), y1 = fminf(y + h, l->crop[1] + l->crop[3]);
    if (x0 >= x1 || y0 >= y1) return;
    float fx = sw / w, fy = sh / h;
    float s0x = sx + (x0 - x) * fx, s0y = sy + (y0 - y) * fy, s1x = sx + (x1 - x) * fx, s1y = sy + (y1 - y) * fy;
    int flags = l->additive ? GFX_BLEND_ADD : GFX_BLEND_NORMAL;
    for (uint8_t i = 0; i < t->ntiles; i++) {
        const vn_tile *tl = &t->tiles[i];
        float tsx0 = tl->x / ktx, tsy0 = tl->y / kty, tsx1 = (tl->x + tl->w) / ktx, tsy1 = (tl->y + tl->h) / kty;
        float ix0 = fmaxf(s0x, tsx0), iy0 = fmaxf(s0y, tsy0), ix1 = fminf(s1x, tsx1), iy1 = fminf(s1y, tsy1);
        if (ix0 >= ix1 || iy0 >= iy1) continue;
        float uv[8], xy[8];
        float u0 = ix0 * ktx - tl->x, v0 = iy0 * kty - tl->y, u1 = ix1 * ktx - tl->x, v1 = iy1 * kty - tl->y;
        uv[0] = u0; uv[1] = v0; uv[2] = u1; uv[3] = v0; uv[4] = u1; uv[5] = v1; uv[6] = u0; uv[7] = v1;
        float cx0 = x0 + (ix0 - s0x) / fx - l->crop[0], cy0 = y0 + (iy0 - s0y) / fy - l->crop[1];
        float cx1 = x0 + (ix1 - s0x) / fx - l->crop[0], cy1 = y0 + (iy1 - s0y) / fy - l->crop[1];
        if (l->native) {
            /* Whole texels land on whole pixels. */
            cx0 = floorf(cx0 * ktx + 0.5f) / ktx;
            cy0 = floorf(cy0 * kty + 0.5f) / kty;
            cx1 = floorf(cx1 * ktx + 0.5f) / ktx;
            cy1 = floorf(cy1 * kty + 0.5f) / kty;
        }
        map_pt(m, tx, ty, cx0, cy0, &xy[0], &xy[1]);
        map_pt(m, tx, ty, cx1, cy0, &xy[2], &xy[3]);
        map_pt(m, tx, ty, cx1, cy1, &xy[4], &xy[5]);
        map_pt(m, tx, ty, cx0, cy1, &xy[6], &xy[7]);
        if (l->native)
            for (int k = 0; k < 8; k++) xy[k] = floorf(xy[k] + 0.5f);
        gfx_draw_quad(tl->tex, xy, uv, mod, flags, NULL);
    }
}

void scene_draw_layout(scene_ctx *c, const layout *l, float dx, float dy, float alpha)
{
    if (!l->visible) return;
    const vn_disp *d = &c->s->disps[l->leaf];
    float a = l->alpha * alpha;
    if (a <= 0.0f) return;
    uint32_t mod = gfx_rgba(255, 255, 255, (uint32_t)(a * 255.0f + 0.5f));
    switch (d->kind) {
    case DISP_TEXTURE: {
        vn_texture *t = d->asset != VN_NONE16 ? vn_assets_texture(c->assets, d->asset) : NULL;
        if (t) draw_region(t, 0, 0, t->src_w, t->src_h, 0, 0, l->child_w, l->child_h, l, dx, dy, mod);
        break;
    }
    case DISP_FRAME: {
        vn_texture *t = d->asset != VN_NONE16 ? vn_assets_texture(c->assets, d->asset) : NULL;
        if (!t) break;
        float sw = t->src_w, sh = t->src_h, W = l->child_w, H = l->child_h;
        float bl = d->borders[0], bt = d->borders[1], br = d->borders[2], bb = d->borders[3];
        if (bl + br > W) bl = br = W / 2;
        if (bt + bb > H) bt = bb = H / 2;
        float sxs[4] = {0, bl, sw - br, sw}, sys[4] = {0, bt, sh - bb, sh};
        float dxs[4] = {0, bl, W - br, W}, dys[4] = {0, bt, H - bb, H};
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 3; i++)
                draw_region(t, sxs[i], sys[j], sxs[i + 1] - sxs[i], sys[j + 1] - sys[j], dxs[i], dys[j],
                            dxs[i + 1] - dxs[i], dys[j + 1] - dys[j], l, dx, dy, mod);
        break;
    }
    case DISP_SOLID: {
        uint32_t ca = (uint32_t)(d->color[3] * a + 0.5f);
        float xs[4], ys[4];
        float pts[4][2] = {{0, 0}, {l->crop[2], 0}, {l->crop[2], l->crop[3]}, {0, l->crop[3]}};
        for (int i = 0; i < 4; i++) map_pt(l->m, l->tx + dx, l->ty + dy, pts[i][0], pts[i][1], &xs[i], &ys[i]);
        float x0 = fminf(fminf(xs[0], xs[1]), fminf(xs[2], xs[3])), x1 = fmaxf(fmaxf(xs[0], xs[1]), fmaxf(xs[2], xs[3]));
        float y0 = fminf(fminf(ys[0], ys[1]), fminf(ys[2], ys[3])), y1 = fmaxf(fmaxf(ys[0], ys[1]), fmaxf(ys[2], ys[3]));
        gfx_fill_rect(x0, y0, x1 - x0, y1 - y0, gfx_rgba(d->color[0], d->color[1], d->color[2], ca));
        break;
    }
    case DISP_TEXT:
        if (c->draw_text) {
            layout tl = *l;
            tl.tx += dx;
            tl.ty += dy;
            tl.alpha = a;
            c->draw_text(c->user, l->leaf, &tl);
        }
        break;
    default:
        break;
    }
}

static const sprite *find_const(const scene *sc, const char *key)
{
    for (uint32_t i = 0; i < sc->n; i++)
        if (strcmp(sc->items[i].key, key) == 0) return &sc->items[i];
    return NULL;
}

void scene_draw(scene_ctx *c, const scene *sc, float dx, float dy, const move_anim *move)
{
    for (uint32_t i = 0; i < sc->n; i++) {
        layout l;
        scene_layout(c, &sc->items[i], &l, NULL);
        if (move) {
            const sprite *o = find_const(move->old, sc->items[i].key);
            if (o) {
                layout ol;
                scene_layout(c, o, &ol, NULL);
                if (ol.visible) {
                    l.tx = ol.tx + (l.tx - ol.tx) * move->f;
                    l.ty = ol.ty + (l.ty - ol.ty) * move->f;
                    if (l.native) {
                        l.tx = floorf(l.tx + 0.5f);
                        l.ty = floorf(l.ty + 0.5f);
                    }
                }
            } else if (move->enter != VN_NONE16) {
                layout el;
                layout_with(c, &sc->items[i], move->enter, &el, NULL);
                l.tx = el.tx + (l.tx - el.tx) * move->fe;
                l.ty = el.ty + (l.ty - el.ty) * move->fe;
            }
        }
        scene_draw_layout(c, &l, dx, dy, 1.0f);
    }
    if (move && move->leave != VN_NONE16) {
        for (uint32_t i = 0; i < move->old->n; i++) {
            const sprite *o = &move->old->items[i];
            if (find_const(sc, o->key)) continue;
            layout l, ll;
            scene_layout(c, o, &l, NULL);
            layout_with(c, o, move->leave, &ll, NULL);
            l.tx += (ll.tx - l.tx) * move->fl;
            l.ty += (ll.ty - l.ty) * move->fl;
            scene_draw_layout(c, &l, dx, dy, 1.0f);
        }
    }
}

void scene_draw_disp_at(scene_ctx *c, uint16_t disp, float x, float y, float alpha)
{
    if (disp >= c->s->ndisps) return;
    sprite sp;
    memset(&sp, 0, sizeof sp);
    sp.disp = disp;
    sp.atl = VN_NONE16;
    sp.shown_at = c->clock;
    layout l;
    scene_layout(c, &sp, &l, NULL);
    if (!l.visible) return;
    /* Place by the drawn size: (x, y) is the bottom-left corner on screen. */
    l.tx = floorf(x + 0.5f);
    l.ty = floorf(y - l.crop[3] * c->v.k + 0.5f);
    scene_draw_layout(c, &l, 0, 0, alpha);
}
