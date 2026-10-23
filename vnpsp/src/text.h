/* Dialogue text: [interpolation], {text tags}, line breaking for Latin and CJK,
 * typewriter pause points, and drawing with the FNT1 glyph atlases. */
#ifndef VN_TEXT_H
#define VN_TEXT_H

#include "assets.h"
#include "value.h"

enum { TS_BOLD = 1, TS_ITALIC = 2, TS_UNDERLINE = 4, TS_STRIKE = 8 };
enum { TEV_WAIT, TEV_FAST, TEV_CPS };

typedef struct tglyph {
    const vn_font *font;
    const vn_glyph *g;        /* NULL for spaces drawn as advance only */
    uint32_t color;           /* 0xAABBGGRR, alpha already includes {alpha} */
    uint8_t style;
    float scale;
    float x, y;               /* pen position: left edge, baseline */
    float adv;
} tglyph;

typedef struct tevent {
    uint32_t at;              /* glyph index the event sits before */
    uint8_t kind;
    float arg;                /* WAIT: seconds, < 0 for a click. CPS: chars per second (0 instant),
                                 -1 back to the setting, <= -2 multiplier -(arg + 2) */
} tevent;

typedef struct text_layout {
    tglyph *g;
    uint32_t n, cap;
    tevent *ev;
    uint32_t nev, evcap;
    float width, height;
    uint32_t nlines;
    bool no_wait;             /* {nw} */
} text_layout;

typedef struct text_style {
    const vn_assets *assets;
    const vn_font *font;
    uint32_t color;
    uint32_t outline_color;
    float max_width;          /* 0 = no wrapping */
    bool center;
    float base_size;          /* game pixel size that {size=N} is relative to */
} text_style;

void text_layout_init(text_layout *tl);
void text_layout_free(text_layout *tl);
/* Rebuilds tl from s; `extend` passes the concatenated text and reveals from the old count. */
void text_layout_build(text_layout *tl, const char *s, size_t len, const text_style *st);
/* Draws glyphs [0, count) with the top-left of the text at (x, y). */
void text_draw(const text_layout *tl, float x, float y, uint32_t count, float alpha, uint32_t outline_color);

/* Width of a plain single line (no tags) in pixels. */
float text_measure_plain(const vn_font *f, const char *s);
/* Draws plain text (no tags, no wrapping) and returns its width. */
float text_draw_plain(const vn_font *f, const char *s, float x, float y, uint32_t color);

/* Ren'Py [field] substitution; `lookup` returns a new reference for a name or false. */
typedef bool (*text_lookup_fn)(void *user, const char *name, size_t len, value *out);
bool text_interpolate(const char *s, size_t len, text_lookup_fn lookup, void *user, vn_buf *out, verr *e);

/* Distinct code points that had no glyph so far (drawn as '?'). */
uint32_t text_missing_glyphs(void);

/* Text with tags removed and escapes resolved (history, traces). */
void text_plain(const char *s, size_t len, vn_buf *out);

#endif
