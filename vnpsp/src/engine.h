/* vnpsp engine: boots a converted game (script.vnb + data.pak), runs the story
 * one frame at a time from button input, and draws through gfx. Platform code
 * (main.c) owns the window, audio thread and frame pacing. */
#ifndef VN_ENGINE_H
#define VN_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio.h"

/* Same bit values as the platform layer's PLAT_BTN_*. */
enum {
    VN_UP = 1u << 0, VN_DOWN = 1u << 1, VN_LEFT = 1u << 2, VN_RIGHT = 1u << 3,
    VN_CROSS = 1u << 4, VN_CIRCLE = 1u << 5, VN_SQUARE = 1u << 6, VN_TRIANGLE = 1u << 7,
    VN_L = 1u << 8, VN_R = 1u << 9, VN_START = 1u << 10, VN_SELECT = 1u << 11
};

typedef struct vn_input {
    uint32_t held, pressed;
    float stick_x, stick_y;
} vn_input;

typedef struct vn_config {
    const char *data_dir;      /* script.vnb and data.pak */
    const char *save_dir;      /* saves, persistent data, settings */
    bool deterministic;        /* fixed random seed and save timestamps (tests) */
    size_t texture_budget;     /* bytes of decoded images kept in RAM */
    void (*trace)(void *user, const char *line);
    void *trace_user;
} vn_config;

typedef enum vn_mode {
    MODE_MAIN_MENU, MODE_RUN, MODE_SAY, MODE_MENU, MODE_PAUSE, MODE_TRANSITION, MODE_INPUT,
    MODE_GAME_MENU, MODE_SAVE, MODE_LOAD, MODE_SETTINGS, MODE_HISTORY, MODE_CONFIRM, MODE_ERROR, MODE_QUIT
} vn_mode;

typedef struct vn_engine vn_engine;

vn_engine *vn_engine_new(const vn_config *cfg, char *err, size_t errcap);
void vn_engine_free(vn_engine *e);

/* One frame of logic (dt seconds), then drawing of that frame. */
void vn_engine_update(vn_engine *e, const vn_input *in, float dt);
void vn_engine_render(vn_engine *e);

vn_mixer *vn_engine_mixer(vn_engine *e);
const char *vn_engine_title(const vn_engine *e);

/* Introspection for the headless driver and tests. */
vn_mode vn_engine_mode(const vn_engine *e);
int vn_engine_cursor(const vn_engine *e, int *count);   /* menus and lists */
bool vn_engine_text_complete(const vn_engine *e);        /* the say text is fully shown */
const char *vn_engine_error(const vn_engine *e);
uint64_t vn_engine_state_hash(vn_engine *e);             /* hash of the story state (checkpoint) */
int vn_engine_rollback_depth(const vn_engine *e);
/* Entries read from data.pak, their bytes, and bytes of decoded images cached now. */
void vn_engine_asset_stats(const vn_engine *e, uint32_t *loads, uint64_t *bytes, size_t *cached);
/* Counts interactions started (lines, menus, click pauses, inputs) since boot. */
uint32_t vn_engine_interactions(const vn_engine *e);
/* On-screen keyboard: grid cell of a character (row, column), false if absent. */
bool vn_engine_osk_locate(char ch, int *row, int *col);
void vn_engine_osk_cursor(const vn_engine *e, int *row, int *col);
const char *vn_engine_osk_text(const vn_engine *e);

#endif
