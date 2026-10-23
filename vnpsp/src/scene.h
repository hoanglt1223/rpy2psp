/* The master layer: shown images keyed by tag, their transforms, placement in
 * source pixels, and drawing onto the 480x272 frame (uniform scale plus
 * letterbox, bytecode.md "Program model"). */
#ifndef VN_SCENE_H
#define VN_SCENE_H

#include "assets.h"
#include "atl.h"
#include "vnb.h"

#define SCENE_KEY_MAX 48
#define SCENE_AT_MAX 4

typedef struct sprite {
    char key[SCENE_KEY_MAX];   /* layer key: the tag, or the `as` name */
    uint16_t image;            /* IMGS index, VN_NONE16 for a bare displayable */
    uint16_t disp;             /* displayable drawn (from the image or the show record) */
    uint8_t nat;
    uint16_t at[SCENE_AT_MAX];
    uint16_t atl;
    int32_t zorder;
    double shown_at;           /* scene clock when the transform started */
} sprite;

typedef struct scene {
    sprite *items;
    uint32_t n, cap;
} scene;

/* Placement of one sprite: child pixel (x, y) inside the crop maps to screen
 * (tx + m0*x + m1*y, ty + m2*x + m3*y). */
typedef struct layout {
    bool visible;
    uint16_t leaf;             /* displayable actually drawn */
    float child_w, child_h;    /* source pixels */
    float crop[4];             /* x, y, w, h in child pixels */
    float m[4], tx, ty;
    float alpha;
    bool additive;
    bool native;               /* unscaled and unrotated: draw textures at their own pixel size */
} layout;

typedef struct view {
    float k, ox, oy;           /* screen = source * k + offset */
} view;

typedef struct scene_ctx {
    const vn_script *s;
    vn_assets *assets;
    view v;
    double clock;              /* seconds */
    void (*draw_text)(void *user, uint16_t disp, const layout *l);
    void (*measure_text)(void *user, uint16_t disp, float *w, float *h);
    void *user;
} scene_ctx;

void scene_init(scene *sc);
void scene_free(scene *sc);
void scene_clear(scene *sc);
void scene_copy(scene *dst, const scene *src);
sprite *scene_find(scene *sc, const char *key);

view scene_view(const vn_script *s);

/* Ren'Py's attribute rule: the image of `tag` that has every requested
 * attribute and the most of the current ones. Returns an IMGS index or VN_NONE16. */
uint16_t scene_resolve(const vn_script *s, const char *tag, const char *const *attrs, int nattrs, uint16_t current);

/* SHOW/SCENE/HIDE. name/attrs override the record's (show expression). */
void scene_show(scene_ctx *c, scene *sc, const vn_show *rec, const char *tag, const char *const *attrs, int nattrs);
void scene_hide(scene *sc, const char *key);
/* Re-shows a tag with changed attributes (say attributes), keeping its transform. */
bool scene_set_attrs(scene_ctx *c, scene *sc, const char *tag, const char *const *attrs, int nattrs);
bool scene_showing(const vn_script *s, scene *sc, const char *name);

void scene_layout(scene_ctx *c, const sprite *sp, layout *out, const char *event);
void scene_draw_layout(scene_ctx *c, const layout *l, float dx, float dy, float alpha);

typedef struct move_anim {
    const scene *old;
    float f;                   /* 0..1, already warped */
    uint16_t enter, leave;     /* transforms, VN_NONE16 for none */
    float fe, fl;              /* warped progress for entering and leaving images */
} move_anim;

/* Draws every sprite; dx/dy shift the whole layer in screen pixels. */
void scene_draw(scene_ctx *c, const scene *sc, float dx, float dy, const move_anim *move);

/* Draws a displayable as the side image: bottom-left of the screen. */
void scene_draw_disp_at(scene_ctx *c, uint16_t disp, float x, float y, float alpha);

#endif
