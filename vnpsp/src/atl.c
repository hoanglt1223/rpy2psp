#include "atl.h"

#include <math.h>
#include <string.h>

#define PI_F 3.14159265358979f

typedef struct ctx {
    const vn_script *s;
    atl_props *p;
    const atl_env *env;
    int depth;
} ctx;

void atl_props_init(atl_props *p)
{
    memset(p, 0, sizeof *p);
    p->child = VN_NONE16;
}

float atl_warp(int warper, float t)
{
    t = vn_clampf(t, 0.0f, 1.0f);
    switch (warper) {
    case 1: return 0.5f - cosf(PI_F * t) / 2.0f;
    case 2: return cosf((1.0f - t) * PI_F / 2.0f);
    case 3: return 1.0f - cosf(t * PI_F / 2.0f);
    default: return t;
    }
}

float atl_px(atl_val v, float area)
{
    if (v.kind == AV_PX) return v.v;
    if (v.kind == AV_NONE) return 0.0f;
    return v.v * area;
}

/* Value a property has before anything set it. */
static atl_val default_of(int slot)
{
    atl_val v = {0.0f, AV_FLOAT};
    switch (slot) {
    case P_ZOOM: case P_XZOOM: case P_YZOOM: case P_ALPHA: v.v = 1.0f; break;
    case P_ROTATE: v.kind = AV_NONE; break;
    case P_XOFFSET: case P_YOFFSET: v.kind = AV_PX; break;
    case P_CROP_W: case P_CROP_H: v.v = 1.0f; break;
    case P_XSIZE: case P_YSIZE: v.kind = AV_NONE; break;
    default: break;
    }
    return v;
}

static atl_val get(const atl_props *p, int slot)
{
    return (p->set >> slot) & 1 ? p->v[slot] : default_of(slot);
}

static void put(atl_props *p, int slot, atl_val v)
{
    p->v[slot] = v;
    p->set |= 1u << slot;
}

static float area_of(const ctx *c, int slot)
{
    switch (slot) {
    case P_XPOS: case P_XOFFSET: return c->env->screen_w;
    case P_YPOS: case P_YOFFSET: return c->env->screen_h;
    case P_XANCHOR: case P_CROP: case P_CROP_W: case P_XSIZE: return c->env->child_w;
    case P_YANCHOR: case P_CROP_Y: case P_CROP_H: case P_YSIZE: return c->env->child_h;
    default: return 1.0f;
    }
}

/* Interpolates one slot from its current value toward `to` by fraction f. */
static void blend_slot(ctx *c, int slot, atl_val to, float f)
{
    atl_val from = get(c->p, slot);
    atl_val out = to;
    if (to.kind == AV_NONE || from.kind == AV_NONE) {
        out = f >= 1.0f ? to : from;
    } else if (from.kind == to.kind) {
        out.v = from.v + (to.v - from.v) * f;
    } else {
        float area = area_of(c, slot);
        out.kind = AV_PX;
        out.v = atl_px(from, area) + (atl_px(to, area) - atl_px(from, area)) * f;
    }
    put(c->p, slot, out);
}

/* Parses a property list at code[*pos] and sets (f >= 1) or interpolates it. */
static bool props(ctx *c, const uint8_t *code, uint32_t len, uint32_t *pos, float f, bool interp)
{
    if (*pos >= len) return false;
    uint8_t count = code[(*pos)++];
    for (uint8_t i = 0; i < count; i++) {
        if (*pos + 2 > len) return false;
        uint8_t prop = code[*pos], n = code[*pos + 1];
        *pos += 2;
        if (*pos + 5u * n > len) return false;
        atl_val comps[4] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
        for (uint8_t k = 0; k < n; k++) {
            if (k < 4) {
                comps[k].kind = code[*pos];
                comps[k].v = vn_lef32(code + *pos + 1);
            }
            *pos += 5;
        }
        int slots[4] = {-1, -1, -1, -1};
        atl_val vals[4] = {comps[0], comps[0], comps[0], comps[0]};
        switch (prop) {
        case P_XALIGN: slots[0] = P_XPOS; slots[1] = P_XANCHOR; break;
        case P_YALIGN: slots[0] = P_YPOS; slots[1] = P_YANCHOR; break;
        case P_XCENTER: slots[0] = P_XPOS; slots[1] = P_XANCHOR; vals[1].v = 0.5f; vals[1].kind = AV_FLOAT; break;
        case P_YCENTER: slots[0] = P_YPOS; slots[1] = P_YANCHOR; vals[1].v = 0.5f; vals[1].kind = AV_FLOAT; break;
        case P_CROP:
            if (n < 4) continue;
            slots[0] = P_CROP; slots[1] = P_CROP_Y; slots[2] = P_CROP_W; slots[3] = P_CROP_H;
            for (int k = 0; k < 4; k++) vals[k] = comps[k];
            break;
        default:
            if (prop > P_YCENTER) continue;
            slots[0] = prop;
            break;
        }
        for (int k = 0; k < 4 && slots[k] >= 0; k++) {
            if (interp)
                blend_slot(c, slots[k], vals[k], f);
            else
                put(c->p, slots[k], vals[k]);
        }
    }
    return true;
}

static uint32_t mix(uint32_t a, uint32_t b)
{
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

static bool run_block(ctx *c, const uint8_t *code, uint32_t len, double *remain);

/* Returns false while the block is still running at the requested time;
 * *remain is how much of the requested time is left after the block. */
static bool run_block(ctx *c, const uint8_t *code, uint32_t len, double *remain)
{
    if (++c->depth > 32) {
        c->depth--;
        return true;
    }
    int32_t done_runs = 0;
    bool fast_forwarded = false;
    for (;;) {
        double start = *remain;
        uint32_t pos = 0;
        bool restart = false;
        bool on_matched = false;
        while (pos < len && !restart) {
            uint8_t op = code[pos++];
            switch (op) {
            case 0x01:
                if (!props(c, code, len, &pos, 1.0f, false)) goto out;
                break;
            case 0x02: {
                if (pos + 5 > len) goto out;
                uint8_t warper = code[pos];
                float dur = vn_lef32(code + pos + 1);
                pos += 5;
                if (!(dur > 0.0f)) {
                    if (!props(c, code, len, &pos, 1.0f, false)) goto out;
                    break;
                }
                if (*remain < dur) {
                    float f = atl_warp(warper, (float)(*remain / dur));
                    props(c, code, len, &pos, f, true);
                    *remain = 0;
                    c->depth--;
                    return false;
                }
                if (!props(c, code, len, &pos, 1.0f, true)) goto out;
                *remain -= dur;
                break;
            }
            case 0x03: case 0x09: {
                if (pos + 4 > len) goto out;
                float d = vn_lef32(code + pos);
                pos += 4;
                if (op == 0x09) d = d - (float)(start - *remain);   /* `time t`: wait until t into the block */
                if (d <= 0.0f) break;
                if (*remain < d) {
                    *remain = 0;
                    c->depth--;
                    return false;
                }
                *remain -= d;
                break;
            }
            case 0x04: {
                if (pos + 4 > len) goto out;
                int32_t n = (int32_t)vn_le32(code + pos);
                pos += 4;
                done_runs++;
                if (n >= 0 && done_runs >= n) break;
                double pass = start - *remain;
                if (pass <= 0.0) goto out;   /* a zero-length loop would never advance */
                if (n < 0 && !fast_forwarded && *remain > 2.0 * pass) {
                    /* Every pass after the first restarts from the same state, so skip whole passes. */
                    *remain -= floor(*remain / pass - 1.0) * pass;
                    fast_forwarded = true;
                }
                restart = true;
                break;
            }
            case 0x05: {
                if (pos + 4 > len) goto out;
                uint32_t blen = vn_le32(code + pos);
                pos += 4;
                if (blen > len - pos) goto out;
                if (!run_block(c, code + pos, blen, remain)) {
                    c->depth--;
                    return false;
                }
                pos += blen;
                break;
            }
            case 0x06: {
                if (pos >= len) goto out;
                uint8_t n = code[pos++];
                double least = *remain;
                bool all = true;
                for (uint8_t i = 0; i < n; i++) {
                    if (pos + 4 > len) goto out;
                    uint32_t blen = vn_le32(code + pos);
                    pos += 4;
                    if (blen > len - pos) goto out;
                    double r = *remain;
                    if (!run_block(c, code + pos, blen, &r)) all = false;
                    if (r < least) least = r;
                    pos += blen;
                }
                if (!all) {
                    *remain = 0;
                    c->depth--;
                    return false;
                }
                *remain = least;
                break;
            }
            case 0x07: {
                if (pos >= len) goto out;
                uint8_t n = code[pos++];
                uint32_t p0 = pos;
                float total = 0;
                for (uint8_t i = 0; i < n; i++) {
                    if (pos + 8 > len) goto out;
                    total += vn_lef32(code + pos);
                    uint32_t blen = vn_le32(code + pos + 4);
                    if (blen > len - pos - 8) goto out;
                    pos += 8 + blen;
                }
                float pick = (float)(mix(c->env->seed, p0 + (uint32_t)done_runs) % 10000u) / 10000.0f * total;
                uint32_t q = p0;
                for (uint8_t i = 0; i < n; i++) {
                    float w = vn_lef32(code + q);
                    uint32_t blen = vn_le32(code + q + 4);
                    if (pick < w || i + 1 == n) {
                        if (!run_block(c, code + q + 8, blen, remain)) {
                            c->depth--;
                            return false;
                        }
                        break;
                    }
                    pick -= w;
                    q += 8 + blen;
                }
                break;
            }
            case 0x08: {
                if (pos >= len) goto out;
                uint8_t n = code[pos++];
                if (pos + 4u * n + 4 > len) goto out;
                bool match = false;
                for (uint8_t i = 0; i < n; i++) {
                    const char *name = vn_str(c->s, vn_le32(code + pos + 4 * i));
                    if (strcmp(name, "start") == 0 || (c->env->event && strcmp(name, c->env->event) == 0))
                        match = true;
                }
                pos += 4u * n;
                uint32_t blen = vn_le32(code + pos);
                pos += 4;
                if (blen > len - pos) goto out;
                if (match && !on_matched) {
                    on_matched = true;
                    if (!run_block(c, code + pos, blen, remain)) {
                        c->depth--;
                        return false;
                    }
                }
                pos += blen;
                break;
            }
            case 0x0A:
                if (pos + 4 > len) goto out;
                c->p->child = vn_le16(code + pos);
                pos += 4;
                break;
            case 0x0B:
                pos += 4;
                break;
            default:
                goto out;
            }
        }
        if (!restart) break;
    }
out:
    c->depth--;
    return true;
}

bool atl_apply(const vn_script *s, uint16_t id, double t, atl_props *p, const atl_env *env)
{
    if (id >= s->ntransforms) return true;
    ctx c = {s, p, env, 0};
    double remain = t < 0 ? 0 : t;
    return run_block(&c, s->transforms[id].code, s->transforms[id].len, &remain);
}

double atl_duration(const vn_script *s, uint16_t id, const atl_env *env)
{
    if (id >= s->ntransforms) return 0;
    atl_props p;
    atl_props_init(&p);
    ctx c = {s, &p, env, 0};
    const double horizon = 1e7;
    double remain = horizon;
    if (!run_block(&c, s->transforms[id].code, s->transforms[id].len, &remain)) return -1;
    return horizon - remain;
}
