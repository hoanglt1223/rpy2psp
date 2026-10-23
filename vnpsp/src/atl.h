/* ATL subset (bytecode.md, Transforms). A transform's state at time t is
 * computed by replaying its program from 0, so it depends only on (program,
 * elapsed time, starting properties): saves and rollback store just the time. */
#ifndef VN_ATL_H
#define VN_ATL_H

#include "vnb.h"

enum {
    P_XPOS, P_YPOS, P_XANCHOR, P_YANCHOR, P_XALIGN, P_YALIGN, P_XOFFSET, P_YOFFSET, P_ZOOM, P_XZOOM, P_YZOOM,
    P_ALPHA, P_ROTATE, P_ADDITIVE, P_XSIZE, P_YSIZE, P_CROP, P_XCENTER, P_YCENTER,
    P_CROP_Y = 19, P_CROP_W, P_CROP_H, P_SLOTS
};

enum { AV_FLOAT = 0, AV_PX = 1, AV_NONE = 2 };

typedef struct atl_val {
    float v;
    uint8_t kind;
} atl_val;

typedef struct atl_props {
    uint32_t set;             /* bit per slot; xalign/xcenter write xpos and xanchor */
    atl_val v[P_SLOTS];
    uint16_t child;           /* CHILD displayable, VN_NONE16 if none */
} atl_props;

typedef struct atl_env {
    float screen_w, screen_h; /* source pixels: the area positions are fractions of */
    float child_w, child_h;   /* anchors and crop fractions refer to the child */
    uint32_t seed;            /* picks `choice` branches deterministically */
    const char *event;        /* "show", "replace", ... for `on` handlers */
} atl_env;

void atl_props_init(atl_props *p);
/* Applies transform `id` at time t on top of p. Returns true when the program
 * has finished (no more change after t). */
bool atl_apply(const vn_script *s, uint16_t id, double t, atl_props *p, const atl_env *env);
/* Total run time of a non-repeating transform, or a negative value if it repeats forever. */
double atl_duration(const vn_script *s, uint16_t id, const atl_env *env);
float atl_warp(int warper, float t);
/* Resolves a value to source pixels against an area size. */
float atl_px(atl_val v, float area);

#endif
