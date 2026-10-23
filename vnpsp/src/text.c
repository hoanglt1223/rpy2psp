#include "text.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t missing[64];
static uint32_t nmissing;

/* Remembers code points the fonts lack (they draw as '?'), for tests and logs. */
static void note_missing(uint32_t cp)
{
    for (uint32_t i = 0; i < nmissing; i++)
        if (missing[i] == cp) return;
    if (nmissing < VN_ARRAY_LEN(missing)) missing[nmissing++] = cp;
    vn_log("text: no glyph for U+%04X", (unsigned)cp);
}

uint32_t text_missing_glyphs(void)
{
    return nmissing;
}

void text_layout_init(text_layout *tl)
{
    memset(tl, 0, sizeof *tl);
}

void text_layout_free(text_layout *tl)
{
    free(tl->g);
    free(tl->ev);
    memset(tl, 0, sizeof *tl);
}

/* ---- interpolation ---------------------------------------------------------------- */

static bool apply_key(value *cur, const char *k, size_t n, verr *e)
{
    while (n && k[0] == ' ') {
        k++;
        n--;
    }
    while (n && k[n - 1] == ' ') n--;
    bool digits = n > 0;
    for (size_t i = 0; i < n; i++)
        if (k[i] < '0' || k[i] > '9') digits = false;
    value key;
    if (n >= 2 && (k[0] == '\'' || k[0] == '"') && k[n - 1] == k[0]) {
        key = v_str(k + 1, n - 2);
    } else if (digits) {
        int64_t iv = 0;
        for (size_t i = 0; i < n; i++) iv = iv * 10 + (k[i] - '0');
        key = v_int(iv);
    } else {
        key = v_str(k, n);
    }
    value out;
    bool ok = v_subscr(*cur, key, &out, e);
    v_dec(key);
    v_dec(*cur);
    *cur = ok ? out : v_none();
    return ok;
}

static bool interp_field(const char *f, size_t n, text_lookup_fn lookup, void *user, vn_buf *out, verr *e, int depth)
{
    size_t i = 0;
    while (i < n && f[i] == ' ') i++;
    size_t name0 = i;
    while (i < n && f[i] != '[' && f[i] != '!' && f[i] != ':' && f[i] != ' ') i++;
    value cur;
    if (!lookup(user, f + name0, i - name0, &cur))
        return verr_set(e, "NameError: [%.*s] names no variable", (int)(i - name0), f + name0);
    while (i < n && f[i] == '[') {
        size_t j = i + 1;
        while (j < n && f[j] != ']') j++;
        if (j >= n) {
            v_dec(cur);
            return verr_set(e, "bad interpolation [%.*s]", (int)n, f);
        }
        if (!apply_key(&cur, f + i + 1, j - i - 1, e)) return false;
        i = j + 1;
    }
    char conv[16] = {0};
    size_t nc = 0;
    while (i < n && f[i] == '!') {
        i++;
        while (i < n && f[i] != '!' && f[i] != ':' && f[i] != ' ' && nc < sizeof conv - 1) conv[nc++] = f[i++];
    }
    const char *spec = "";
    size_t speclen = 0;
    if (i < n && f[i] == ':') {
        spec = f + i + 1;
        speclen = n - i - 1;
        while (speclen && spec[speclen - 1] == ' ') speclen--;
    }
    if (nc) {
        vn_buf b = {0};
        if (strchr(conv, 'r'))
            v_repr(cur, &b);
        else
            v_to_str(cur, &b);
        if (strchr(conv, 'i') && depth < 4) {
            vn_buf again = {0};
            bool ok = text_interpolate((const char *)b.data, b.len, lookup, user, &again, e);
            vn_buf_free(&b);
            if (!ok) {
                vn_buf_free(&again);
                v_dec(cur);
                return false;
            }
            b = again;
        }
        if (strchr(conv, 'q')) {
            vn_buf q = {0};
            for (size_t k = 0; k < b.len; k++) {
                vn_buf_u8(&q, b.data[k]);
                if (b.data[k] == '{' || b.data[k] == '[') vn_buf_u8(&q, b.data[k]);
            }
            vn_buf_free(&b);
            b = q;
        }
        for (size_t k = 0; k < b.len; k++) {
            char ch = (char)b.data[k];
            if (strchr(conv, 'u') && ch >= 'a' && ch <= 'z') b.data[k] = (uint8_t)(ch - 32);
            if (strchr(conv, 'l') && ch >= 'A' && ch <= 'Z') b.data[k] = (uint8_t)(ch + 32);
        }
        if (strchr(conv, 'c') && b.len && b.data[0] >= 'a' && b.data[0] <= 'z') b.data[0] = (uint8_t)(b.data[0] - 32);
        v_dec(cur);
        cur = v_str((const char *)b.data, b.len);
        vn_buf_free(&b);
    }
    bool ok = v_format(cur, spec, speclen, out, e);
    v_dec(cur);
    return ok;
}

static bool interp_depth(const char *s, size_t len, text_lookup_fn lookup, void *user, vn_buf *out, verr *e, int depth)
{
    for (size_t i = 0; i < len;) {
        if (s[i] == '[' && i + 1 < len && s[i + 1] == '[') {
            vn_buf_u8(out, '[');
            i += 2;
        } else if (s[i] == '%' && i + 1 < len && s[i + 1] == '%') {
            vn_buf_u8(out, '%');
            i += 2;
        } else if (s[i] == '[') {
            int d = 0;
            size_t j = i;
            for (; j < len; j++) {
                if (s[j] == '[') d++;
                else if (s[j] == ']' && --d == 0) break;
            }
            if (j >= len) {
                vn_buf_put(out, s + i, len - i);
                return true;
            }
            if (!interp_field(s + i + 1, j - i - 1, lookup, user, out, e, depth)) return false;
            i = j + 1;
        } else {
            vn_buf_u8(out, (uint8_t)s[i++]);
        }
    }
    return true;
}

bool text_interpolate(const char *s, size_t len, text_lookup_fn lookup, void *user, vn_buf *out, verr *e)
{
    return interp_depth(s, len, lookup, user, out, e, 0);
}

/* ---- tags ------------------------------------------------------------------- */

typedef struct tag {
    const char *name, *val;
    size_t nlen, vlen;
    bool close;
} tag;

/* Parses "{...}" at s[i]; returns the index after '}' or 0 if it is not a tag. */
static size_t parse_tag(const char *s, size_t len, size_t i, tag *t)
{
    size_t j = i + 1;
    while (j < len && s[j] != '}') j++;
    if (j >= len) return 0;
    const char *body = s + i + 1;
    size_t n = j - i - 1;
    memset(t, 0, sizeof *t);
    if (n && body[0] == '/') {
        t->close = true;
        body++;
        n--;
    }
    const char *eq = memchr(body, '=', n);
    t->name = body;
    t->nlen = eq ? (size_t)(eq - body) : n;
    if (eq) {
        t->val = eq + 1;
        t->vlen = n - t->nlen - 1;
    }
    return j + 1;
}

static bool tag_is(const tag *t, const char *name)
{
    return strlen(name) == t->nlen && memcmp(t->name, name, t->nlen) == 0;
}

void text_plain(const char *s, size_t len, vn_buf *out)
{
    for (size_t i = 0; i < len;) {
        if (s[i] == '{') {
            if (i + 1 < len && s[i + 1] == '{') {
                vn_buf_u8(out, '{');
                i += 2;
                continue;
            }
            tag t;
            size_t next = parse_tag(s, len, i, &t);
            if (next) {
                if (!t.close && (tag_is(&t, "p") || tag_is(&t, "vspace"))) vn_buf_u8(out, '\n');
                i = next;
                continue;
            }
        }
        vn_buf_u8(out, (uint8_t)s[i++]);
    }
}

/* ---- layout ------------------------------------------------------------------ */

typedef struct style_state {
    char name[8];
    uint32_t color;
    uint8_t style;
    float scale, alpha;
    const vn_font *font;
} style_state;

static float parse_float(const char *s, size_t n)
{
    char tmp[32];
    if (n >= sizeof tmp) n = sizeof tmp - 1;
    memcpy(tmp, s, n);
    tmp[n] = 0;
    return (float)atof(tmp);
}

static bool parse_color(const char *s, size_t n, uint32_t *out)
{
    if (n < 1 || s[0] != '#') return false;
    s++;
    n--;
    unsigned v[8];
    if (n != 3 && n != 4 && n != 6 && n != 8) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') v[i] = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v[i] = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v[i] = (unsigned)(c - 'A' + 10);
        else return false;
    }
    unsigned r, g, b, a = 255;
    if (n <= 4) {
        r = v[0] * 17;
        g = v[1] * 17;
        b = v[2] * 17;
        if (n == 4) a = v[3] * 17;
    } else {
        r = v[0] * 16 + v[1];
        g = v[2] * 16 + v[3];
        b = v[4] * 16 + v[5];
        if (n == 8) a = v[6] * 16 + v[7];
    }
    *out = gfx_rgba(r, g, b, a);
    return true;
}

static void push_glyph(text_layout *tl, const tglyph *g)
{
    if (tl->n == tl->cap) {
        tl->cap = tl->cap ? tl->cap * 2 : 64;
        tl->g = vn_realloc(tl->g, sizeof(tglyph) * tl->cap);
    }
    tl->g[tl->n++] = *g;
}

static void push_event(text_layout *tl, uint8_t kind, float arg)
{
    if (tl->nev == tl->evcap) {
        tl->evcap = tl->evcap ? tl->evcap * 2 : 8;
        tl->ev = vn_realloc(tl->ev, sizeof(tevent) * tl->evcap);
    }
    tevent *e = &tl->ev[tl->nev++];
    e->at = tl->n;
    e->kind = kind;
    e->arg = arg;
}

static bool is_cjk(uint32_t c)
{
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x20000 && c <= 0x2FFFF);
}

static bool no_line_start(uint32_t c)
{
    static const uint32_t list[] = {0x3001, 0x3002, 0xFF0C, 0xFF0E, 0x30FB, 0xFF1A, 0xFF1B, 0xFF1F, 0xFF01, 0x30FC,
                                    0x300D, 0x300F, 0xFF09, 0x3011, 0x3009, 0x300B, 0x3015, 0xFF5D, 0x2026, 0x2025,
                                    0x3063, 0x3083, 0x3085, 0x3087, 0x30C3, 0x30E3, 0x30E5, 0x30E7, ',', '.', '!',
                                    '?', ';', ':', ')', ']', '}'};
    for (size_t i = 0; i < VN_ARRAY_LEN(list); i++)
        if (list[i] == c) return true;
    return false;
}

static bool no_line_end(uint32_t c)
{
    static const uint32_t list[] = {0x300C, 0x300E, 0xFF08, 0x3010, 0x3008, 0x300A, 0x3014, 0xFF5B, '(', '[', '{'};
    for (size_t i = 0; i < VN_ARRAY_LEN(list); i++)
        if (list[i] == c) return true;
    return false;
}

#define CP_NEWLINE 0xFFFFFFFEu
#define CP_SPACE_PX 0xFFFFFFFDu

void text_layout_build(text_layout *tl, const char *s, size_t len, const text_style *st)
{
    tl->n = tl->nev = 0;
    tl->no_wait = false;
    tl->width = tl->height = 0;
    tl->nlines = 0;
    uint32_t *cps = NULL;   /* code point per glyph, parallel to tl->g */
    size_t cps_cap = 0;

    style_state stack[16];
    int depth = 0;
    memset(&stack[0], 0, sizeof stack[0]);
    stack[0].color = st->color;
    stack[0].scale = 1.0f;
    stack[0].alpha = 1.0f;
    stack[0].font = st->font;
    float base = st->base_size > 0 ? st->base_size : 22.0f;

#define ADD_CP(cpv, gptr, advv)                                                         \
    do {                                                                                \
        style_state *cs_ = &stack[depth];                                               \
        tglyph tg_ = {0};                                                               \
        tg_.font = cs_->font;                                                           \
        tg_.g = (gptr);                                                                 \
        uint32_t a_ = (uint32_t)((cs_->color >> 24) * vn_clampf(cs_->alpha, 0, 1) + 0.5f); \
        tg_.color = (cs_->color & 0x00FFFFFFu) | (a_ << 24);                              \
        tg_.style = cs_->style;                                                         \
        tg_.scale = cs_->scale;                                                         \
        tg_.adv = (advv);                                                               \
        if (tl->n >= cps_cap) {                                                         \
            cps_cap = cps_cap ? cps_cap * 2 : 64;                                        \
            cps = vn_realloc(cps, sizeof(uint32_t) * cps_cap);                           \
        }                                                                               \
        cps[tl->n] = (cpv);                                                             \
        push_glyph(tl, &tg_);                                                           \
    } while (0)

    for (size_t i = 0; i < len;) {
        if (s[i] == '{' && !(i + 1 < len && s[i + 1] == '{')) {
            tag t;
            size_t next = parse_tag(s, len, i, &t);
            if (next) {
                i = next;
                if (t.nlen && t.name[0] == '#') continue;
                if (t.close) {
                    for (int d = depth; d > 0; d--) {
                        if (strlen(stack[d].name) == t.nlen && memcmp(stack[d].name, t.name, t.nlen) == 0) {
                            depth = d - 1;
                            break;
                        }
                    }
                    if (tag_is(&t, "cps")) push_event(tl, TEV_CPS, -1.0f);
                    continue;
                }
                if (tag_is(&t, "w") || tag_is(&t, "p")) {
                    push_event(tl, TEV_WAIT, t.val ? parse_float(t.val, t.vlen) : -1.0f);
                    if (tag_is(&t, "p")) ADD_CP(CP_NEWLINE, NULL, 0);
                    continue;
                }
                if (tag_is(&t, "nw")) {
                    tl->no_wait = true;
                    continue;
                }
                if (tag_is(&t, "fast")) {
                    push_event(tl, TEV_FAST, 0);
                    continue;
                }
                if (tag_is(&t, "space")) {
                    ADD_CP(CP_SPACE_PX, NULL, t.val ? parse_float(t.val, t.vlen) * stack[depth].scale : 0);
                    continue;
                }
                if (tag_is(&t, "vspace")) {
                    ADD_CP(CP_NEWLINE, NULL, t.val ? parse_float(t.val, t.vlen) : 0);
                    continue;
                }
                if (tag_is(&t, "cps")) {
                    if (t.val && t.vlen && t.val[0] == '*')
                        push_event(tl, TEV_CPS, -2.0f - parse_float(t.val + 1, t.vlen - 1));
                    else
                        push_event(tl, TEV_CPS, t.val ? parse_float(t.val, t.vlen) : 0);
                }
                if (depth + 1 >= (int)VN_ARRAY_LEN(stack)) continue;
                stack[depth + 1] = stack[depth];
                depth++;
                style_state *cs = &stack[depth];
                size_t nn = t.nlen < sizeof cs->name - 1 ? t.nlen : sizeof cs->name - 1;
                memcpy(cs->name, t.name, nn);
                cs->name[nn] = 0;
                if (tag_is(&t, "b")) cs->style |= TS_BOLD;
                else if (tag_is(&t, "i")) cs->style |= TS_ITALIC;
                else if (tag_is(&t, "u")) cs->style |= TS_UNDERLINE;
                else if (tag_is(&t, "s")) cs->style |= TS_STRIKE;
                else if (tag_is(&t, "color") && t.val) {
                    uint32_t c;
                    if (parse_color(t.val, t.vlen, &c)) cs->color = c;
                } else if (tag_is(&t, "alpha") && t.val && t.vlen) {
                    if (t.val[0] == '*') cs->alpha *= parse_float(t.val + 1, t.vlen - 1);
                    else if (t.val[0] == '+' || t.val[0] == '-') cs->alpha += parse_float(t.val, t.vlen);
                    else cs->alpha = parse_float(t.val, t.vlen);
                } else if (tag_is(&t, "size") && t.val && t.vlen) {
                    float cur = cs->scale * base;
                    if (t.val[0] == '*') cur *= parse_float(t.val + 1, t.vlen - 1);
                    else if (t.val[0] == '+' || t.val[0] == '-') cur += parse_float(t.val, t.vlen);
                    else cur = parse_float(t.val, t.vlen);
                    cs->scale = vn_clampf(cur / base, 0.25f, 4.0f);
                } else if (tag_is(&t, "font") && t.val && st->assets) {
                    const vn_font *f = vn_assets_face(st->assets, t.val, t.vlen);
                    if (f) cs->font = f;
                }
                continue;
            }
        }
        if (s[i] == '{') i++;   /* "{{" */
        size_t p = i;
        uint32_t cp = vn_utf8_next(s, len, &p);
        i = p;
        if (cp == '\n') {
            ADD_CP(CP_NEWLINE, NULL, 0);
            continue;
        }
        if (cp == '\r') continue;
        if (cp == '\t') cp = ' ';
        const vn_font *f = stack[depth].font;
        const vn_glyph *g = vn_font_glyph(f, cp);
        if (!g && f != st->font) {
            f = st->font;
            g = vn_font_glyph(f, cp);
        }
        if (!g && cp != ' ') {
            note_missing(cp);
            g = vn_font_glyph(f, '?');
        }
        float adv = g ? g->adv64 / 64.0f * stack[depth].scale : f->size * 0.3f * stack[depth].scale;
        ADD_CP(cp, g, adv);
        tl->g[tl->n - 1].font = f;
    }
#undef ADD_CP

    /* Line breaking. */
    uint32_t *line_of = vn_alloc(sizeof(uint32_t) * (tl->n + 1));
    uint32_t line = 0, line_start = 0;
    int64_t last_break = -1;
    float x = 0;
    for (uint32_t i = 0; i < tl->n; i++) {
        uint32_t cp = cps[i];
        line_of[i] = line;
        if (cp == CP_NEWLINE) {
            line++;
            line_start = i + 1;
            x = 0;
            last_break = -1;
            continue;
        }
        float w = tl->g[i].adv;
        if (st->max_width > 0 && x + w > st->max_width && i > line_start && cp != ' ') {
            uint32_t brk = last_break > (int64_t)line_start ? (uint32_t)last_break : i;
            line++;
            x = 0;
            for (uint32_t k = brk; k < i; k++) {
                line_of[k] = line;
                x += tl->g[k].adv;
            }
            line_of[i] = line;
            line_start = brk;
            last_break = -1;
        }
        x += w;
        uint32_t next = i + 1 < tl->n ? cps[i + 1] : 0;
        if (cp == ' ' || cp == CP_SPACE_PX) {
            last_break = i + 1;
        } else if ((is_cjk(cp) || is_cjk(next)) && next && !no_line_start(next) && !no_line_end(cp) && next != ' ') {
            last_break = i + 1;
        }
    }
    tl->nlines = tl->n ? line + 1 : 0;

    /* Positions: per line width (trailing spaces excluded), height and baseline. */
    float y = 0;
    uint32_t i0 = 0;
    for (uint32_t ln = 0; ln < tl->nlines; ln++) {
        uint32_t i1 = i0;
        while (i1 < tl->n && line_of[i1] == ln) i1++;
        float maxs = 0, extra = 0, wsum = 0, wtrim = 0;
        const vn_font *lf = st->font;
        for (uint32_t k = i0; k < i1; k++) {
            if (cps[k] == CP_NEWLINE) {
                extra += tl->g[k].adv;
                continue;
            }
            if (tl->g[k].scale > maxs) {
                maxs = tl->g[k].scale;
                lf = tl->g[k].font;
            }
            wsum += tl->g[k].adv;
            if (cps[k] != ' ') wtrim = wsum;
        }
        if (maxs == 0) maxs = 1.0f;
        float lh = lf->line_h * maxs, asc = lf->ascent * maxs;
        float off = 0;
        if (st->center) off = ((st->max_width > 0 ? st->max_width : wtrim) - wtrim) / 2.0f;
        float px = off;
        bool leading = true;
        for (uint32_t k = i0; k < i1; k++) {
            /* Spaces that start a wrapped line take no room. */
            if (leading && cps[k] == ' ' && ln > 0 && (k == 0 || cps[k - 1] != CP_NEWLINE)) {
                tl->g[k].adv = 0;
            } else if (cps[k] != CP_NEWLINE) {
                leading = false;
            }
            tl->g[k].x = px;
            tl->g[k].y = y + asc;
            px += tl->g[k].adv;
        }
        if (wtrim > tl->width) tl->width = wtrim;
        y += lh + extra;
        i0 = i1;
    }
    tl->height = y;
    if (st->center && st->max_width > 0) tl->width = st->max_width;
    free(line_of);
    free(cps);
}

/* ---- drawing ------------------------------------------------------------------- */

static void make_clut(uint32_t *clut, int n, uint32_t color, bool high_nibble)
{
    uint32_t a = color >> 24, rgb = color & 0x00FFFFFFu;
    for (int i = 0; i < n; i++) {
        uint32_t level = n == 16 ? (uint32_t)i : (high_nibble ? (uint32_t)i >> 4 : (uint32_t)i & 15);
        clut[i] = rgb | ((a * level * 17 / 255) << 24);
    }
}

static void draw_glyph(const vn_font *f, const vn_glyph *g, float x, float y, float scale, uint32_t color,
                       uint32_t outline, uint8_t style)
{
    float w = g->w * scale, h = g->h * scale;
    float x0 = x + g->bx * scale, y0 = y - g->by * scale;
    if (scale == 1.0f) {
        x0 = floorf(x0 + 0.5f);
        y0 = floorf(y0 + 0.5f);
    }
    float sh = (style & TS_ITALIC) ? h * 0.2f : 0.0f;
    float xy[8] = {x0 + sh, y0, x0 + w + sh, y0, x0 + w, y0 + h, x0, y0 + h};
    float uv[8] = {g->x, g->y, (float)(g->x + g->w), g->y, (float)(g->x + g->w), (float)(g->y + g->h), g->x,
                   (float)(g->y + g->h)};
    gfx_tex *tex = f->pages[g->page];
    uint32_t clut[256];
    int n = f->fmt == GFX_T4 ? 16 : 256;
    int passes = (style & TS_BOLD) ? 2 : 1;
    if (f->fmt == GFX_T8) {
        make_clut(clut, n, (outline & 0x00FFFFFFu) | ((((outline >> 24) * (color >> 24)) / 255) << 24), true);
        for (int p = 0; p < passes; p++) {
            float o[8];
            for (int k = 0; k < 8; k++) o[k] = xy[k] + (k % 2 == 0 ? (float)p : 0.0f);
            gfx_draw_quad(tex, o, uv, 0xFFFFFFFFu, 0, clut);
        }
    }
    make_clut(clut, n, color, false);
    for (int p = 0; p < passes; p++) {
        float o[8];
        for (int k = 0; k < 8; k++) o[k] = xy[k] + (k % 2 == 0 ? (float)p : 0.0f);
        gfx_draw_quad(tex, o, uv, 0xFFFFFFFFu, 0, clut);
    }
}

void text_draw(const text_layout *tl, float x, float y, uint32_t count, float alpha, uint32_t outline_color)
{
    if (count > tl->n) count = tl->n;
    for (uint32_t i = 0; i < count; i++) {
        const tglyph *t = &tl->g[i];
        uint32_t a = (uint32_t)((t->color >> 24) * vn_clampf(alpha, 0, 1) + 0.5f);
        uint32_t c = (t->color & 0x00FFFFFFu) | (a << 24);
        if (t->g && t->g->w && t->g->h) draw_glyph(t->font, t->g, x + t->x, y + t->y, t->scale, c, outline_color, t->style);
        if (t->style & (TS_UNDERLINE | TS_STRIKE) && t->adv > 0) {
            float th = t->scale < 1.5f ? 1.0f : 2.0f;
            if (t->style & TS_UNDERLINE) gfx_fill_rect(x + t->x, y + t->y + 1, t->adv, th, c);
            if (t->style & TS_STRIKE)
                gfx_fill_rect(x + t->x, y + t->y - t->font->ascent * t->scale * 0.35f, t->adv, th, c);
        }
    }
}

float text_measure_plain(const vn_font *f, const char *s)
{
    float w = 0;
    size_t len = strlen(s), p = 0;
    while (p < len) {
        uint32_t cp = vn_utf8_next(s, len, &p);
        const vn_glyph *g = vn_font_glyph(f, cp);
        w += g ? g->adv64 / 64.0f : f->size * 0.3f;
    }
    return w;
}

float text_draw_plain(const vn_font *f, const char *s, float x, float y, uint32_t color)
{
    float pen = 0;
    size_t len = strlen(s), p = 0;
    while (p < len) {
        uint32_t cp = vn_utf8_next(s, len, &p);
        const vn_glyph *g = vn_font_glyph(f, cp);
        if (!g && cp != ' ') {
            note_missing(cp);
            g = vn_font_glyph(f, '?');
        }
        if (g && g->w && g->h) draw_glyph(f, g, x + pen, y + f->ascent, 1.0f, color, gfx_rgba(0, 0, 0, 255), 0);
        pen += g ? g->adv64 / 64.0f : f->size * 0.3f;
    }
    return pen;
}
