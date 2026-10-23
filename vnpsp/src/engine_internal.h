/* State shared by the engine modules: engine.c (story and host instructions),
 * transitions.c, menu.c (choice menu and screens), save.c (checkpoints, saves,
 * persistent data), rollback.c and settings.c. Not a public interface. */
#ifndef VN_ENGINE_INTERNAL_H
#define VN_ENGINE_INTERNAL_H

#include "assets.h"
#include "audio.h"
#include "engine.h"
#include "scene.h"
#include "text.h"
#include "vm.h"

#define HISTORY_MAX 100
#define ROLLBACK_MAX 128
#define ROLLBACK_BYTES (2u << 20)
#define SAVE_SLOTS 30
#define THUMB_W 120
#define THUMB_H 68
#define MENU_MAX 32
#define INPUT_MAX 128

enum { WINDOW_SHOW, WINDOW_HIDE, WINDOW_AUTO };

typedef struct settings {
    float cps;                 /* characters per second, 0 = instant */
    float auto_delay;          /* seconds after a line before auto-forward */
    float vol_music, vol_sfx, vol_voice;
} settings;

typedef struct chan_state {
    uint8_t n;
    uint16_t assets[MIX_PLAYLIST_MAX];
    bool loop;
    float volume;
} chan_state;

typedef struct nvl_line {
    char *who, *text;
    uint32_t color;
} nvl_line;

/* Story state outside the VM; all of it goes into checkpoints. */
typedef struct story {
    scene sc;
    double clock;
    uint8_t window;
    uint16_t last_speaker;
    char *last_text;           /* interpolated text of the last line, for extend */
    bool caption;              /* a non-interacting line waiting for the menu */
    uint16_t caption_char;
    char *caption_text;
    bool revert_pending;       /* say attributes marked temporary */
    char revert_key[SCENE_KEY_MAX];
    uint16_t revert_image;
    nvl_line *nvl;
    uint32_t nnvl;
    chan_state chans[MIX_MAX_CHANNELS];
    bool voice_sustain;
} story;

typedef struct checkpoint {
    uint8_t *data;
    size_t len;
    uint32_t serial;
} checkpoint;

typedef struct history_entry {
    char *who, *text;
    uint32_t serial;
} history_entry;

typedef struct menu_item {
    uint32_t addr;
    char *text;
} menu_item;

struct vn_engine {
    vn_config cfg;
    char *data_dir, *save_dir;
    vn_script script;
    vn_assets assets;
    vm vm;
    vn_mixer *mixer;
    scene_ctx sctx;
    story st;
    scene committed;
    settings set;
    vn_mode mode, overlay_return, ui_parent;
    char err[256];
    uint64_t frame;
    uint32_t interactions;

    /* say */
    text_layout say;
    char *say_who;
    uint32_t say_who_color;
    uint16_t say_char;
    float reveal;              /* glyphs shown, fractional */
    float cps_now;
    uint32_t wait_until;       /* glyph index of a pending {w}/{p}; UINT32_MAX for none */
    float wait_timer;          /* > 0: timed wait remaining */
    bool say_done;
    float auto_timer;
    bool auto_mode, skip, box_hidden;

    /* choice menu */
    menu_item items[MENU_MAX];
    int nitems, cursor;
    float nav_t;               /* held-direction repeat timer */
    uint32_t nav_dir;
    float blink;
    bool say_trans_done;       /* the line's own `with` already ran */
    bool voice_fresh;          /* a voice statement came before this line */
    bool warned_movie;
    uint32_t ev_next;          /* next text event to process */

    /* pause and transition */
    float pause_left;          /* < 0: until click */
    const vn_trans *trans;
    float trans_t, trans_len;
    bool snapshot_pending;

    /* renpy.input */
    char input_prompt[256];
    char input_buf[INPUT_MAX + 1];
    int input_len, input_max, osk_row, osk_col;

    /* checkpoints */
    checkpoint ring[ROLLBACK_MAX];
    int ring_start, ring_count;
    size_t ring_bytes;
    checkpoint current;
    uint32_t serial;
    bool restoring;
    uint64_t persistent_hash;

    history_entry history[HISTORY_MAX];
    int history_n, history_scroll;

    /* screens */
    int ui_cursor, ui_page, confirm_action, confirm_slot, game_menu_cursor;
    char confirm_text[128];
    uint16_t thumb[THUMB_W * THUMB_H];
    bool thumb_valid;
    struct slot_info {
        bool used, valid;
        uint64_t time;
        char desc[64];
        uint16_t thumb[THUMB_W * THUMB_H];
    } slots[SAVE_SLOTS];
    gfx_tex *slot_tex[SAVE_SLOTS];
    uint32_t *slot_rgba[SAVE_SLOTS];
    char message[160];
    float message_t;
    char notify[160];
    float notify_t;
};

/* engine.c */
void eng_trace(vn_engine *e, const char *fmt, ...) VN_PRINTF(2, 3);
/* Appends s with backslash, newline and tab escaped, so a trace field stays on one line. */
void eng_escape(vn_buf *b, const char *s);
void eng_fail(vn_engine *e, const char *fmt, ...) VN_PRINTF(2, 3);
void eng_new_game(vn_engine *e);
void eng_to_main_menu(vn_engine *e);
void eng_apply_audio_state(vn_engine *e, const chan_state *before);
void eng_draw_story(vn_engine *e);
void eng_message(vn_engine *e, const char *msg);
const vn_font *eng_font(vn_engine *e, int role);

/* transitions.c */
void trans_start(vn_engine *e, const vn_trans *t);
bool trans_update(vn_engine *e, float dt);   /* true when finished */
void trans_finish(vn_engine *e);
void trans_draw(vn_engine *e);
float trans_duration(const vn_trans *t);

/* menu.c; nav returns -1/+1 for up/left or down/right with key repeat, 0 otherwise */
int nav_vertical(vn_engine *e, const vn_input *in);
int nav_horizontal(vn_engine *e, const vn_input *in);
void ui_update(vn_engine *e, const vn_input *in);
void ui_draw(vn_engine *e);
void ui_open(vn_engine *e, vn_mode mode);
void choice_update(vn_engine *e, const vn_input *in);
void choice_draw(vn_engine *e);
void osk_update(vn_engine *e, const vn_input *in);
void osk_draw(vn_engine *e);
void draw_panel(float x, float y, float w, float h, uint32_t fill, uint32_t border);

/* save.c */
void ck_serialize(vn_engine *e, vn_buf *out);
bool ck_restore(vn_engine *e, const uint8_t *data, size_t len);
bool save_write(vn_engine *e, const char *name);
bool save_read(vn_engine *e, const char *name, bool apply, struct slot_info *info);
void save_slot_name(int slot, char *out, size_t cap);
void persistent_load(vn_engine *e);
void persistent_save(vn_engine *e, bool force);
void thumb_capture(vn_engine *e);

/* rollback.c */
void rb_checkpoint(vn_engine *e);
bool rb_back(vn_engine *e);
void rb_clear(vn_engine *e);
void rb_free(vn_engine *e);

/* settings.c */
void settings_default(settings *s);
void settings_load(vn_engine *e);
void settings_save(vn_engine *e);
void settings_apply(vn_engine *e);

/* history */
void history_add(vn_engine *e, const char *who, const char *text);
void history_truncate(vn_engine *e, uint32_t serial);
void history_free(vn_engine *e);

#endif
