/* Headless driver (PC): plays a converted game from a command script by
 * pressing buttons frame by frame, with no window and no audio device.
 * It writes the engine trace plus frame/state/audio hashes, so tests can
 * compare runs with the reference interpreter and with each other.
 *
 * Script commands, one per line ('#' starts a comment):
 *   choices N...        menu picks to use, by index among the visible choices
 *   inputs TEXT...      answers for renpy.input ('_' stands for a space)
 *   play [N]            auto-play until the story ends, or N interactions
 *   start               choose Start on the main menu
 *   advance [N]         finish N interactions (says, pauses, menus from the queue)
 *   wait N              N frames without input
 *   press BTN [N]       press and release a button N times (UP DOWN LEFT RIGHT X O
 *                       SQUARE TRIANGLE L R START SELECT)
 *   hold BTN N          hold a button for N frames
 *   hash | state        trace the rendered frame hash / the checkpoint hash
 *   shot FILE           write the current frame as PNG
 *   save N | load N     save to / load from slot N (1-30) through the menus
 *   qsave | qload       quick save (SELECT) / quick load (game menu)
 *   rollback N          press L N times
 *   expect MODE         fail unless the engine is in MODE (say, menu, main, ...)
 */
#include "driver.h"

#include "base.h"
#include "engine.h"
#include "gfx.h"
#include "gfx_soft.h"
#include "png_write.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct drv {
    vn_engine *e;
    FILE *trace;
    uint32_t held_prev;
    long frame, max_frames;
    int choices[4096], nchoices, choice_pos;
    char *inputs[64];
    int ninputs, input_pos;
    bool render_all, fast, ended, failed;
    uint64_t audio_hash;
    uint64_t audio_nonzero, audio_frames;
    bool audio;
    FILE *wav;
    long frames_in_story;
} drv;

static const char *mode_names[] = {"main", "run", "say", "menu", "pause", "transition", "input", "gamemenu",
                                   "save", "load", "settings", "history", "confirm", "error", "quit"};

static void trace_line(void *user, const char *line)
{
    drv *d = user;
    fprintf(d->trace, "%s\n", line);
    if (strcmp(line, "end") == 0) d->ended = true;
    if (strncmp(line, "error\t", 6) == 0) d->failed = true;
}

static void mix_audio(drv *d)
{
    if (!d->audio) return;
    /* 44100 / 60 frames per video frame, spread so whole seconds add up exactly. */
    int n = (int)((d->frame + 1) * 44100 / 60 - d->frame * 44100 / 60);
    int16_t buf[2 * 800];
    mixer_render(vn_engine_mixer(d->e), buf, n);
    for (int i = 0; i < 2 * n; i++) {
        d->audio_hash = (d->audio_hash ^ (uint16_t)buf[i]) * 0x100000001b3ull;
        if (buf[i]) d->audio_nonzero++;
    }
    d->audio_frames += (uint64_t)n;
    if (d->wav) fwrite(buf, sizeof(int16_t), 2 * (size_t)n, d->wav);
}

static bool step(drv *d, uint32_t held, bool render)
{
    if (d->frame >= d->max_frames) {
        fprintf(d->trace, "fail\tframe limit %ld reached in mode %s\n", d->max_frames, mode_names[vn_engine_mode(d->e)]);
        d->failed = true;
        return false;
    }
    vn_input in = {held, held & ~d->held_prev, 0, 0};
    d->held_prev = held;
    vn_engine_update(d->e, &in, 1.0f / 60.0f);
    if (render || d->render_all) vn_engine_render(d->e);
    mix_audio(d);
    d->frame++;
    return !d->failed;
}

static bool press(drv *d, uint32_t btn)
{
    return step(d, btn, false) && step(d, 0, false);
}

static uint32_t button(const char *name)
{
    static const struct { const char *n; uint32_t b; } map[] = {
        {"UP", VN_UP}, {"DOWN", VN_DOWN}, {"LEFT", VN_LEFT}, {"RIGHT", VN_RIGHT}, {"X", VN_CROSS},
        {"O", VN_CIRCLE}, {"SQUARE", VN_SQUARE}, {"TRIANGLE", VN_TRIANGLE}, {"L", VN_L}, {"R", VN_R},
        {"START", VN_START}, {"SELECT", VN_SELECT}};
    for (size_t i = 0; i < VN_ARRAY_LEN(map); i++)
        if (strcmp(map[i].n, name) == 0) return map[i].b;
    return 0;
}

static bool move_cursor(drv *d, int target)
{
    for (int guard = 0; guard < 200; guard++) {
        int count;
        int cur = vn_engine_cursor(d->e, &count);
        if (cur == target) return true;
        if (!press(d, cur < target ? VN_DOWN : VN_UP)) return false;
    }
    fprintf(d->trace, "fail\tcursor did not reach %d\n", target);
    d->failed = true;
    return false;
}

static bool type_text(drv *d, const char *text)
{
    /* The field starts with the script's default; clear it like a player would. */
    for (size_t n = strlen(vn_engine_osk_text(d->e)); n > 0; n--)
        if (!press(d, VN_CIRCLE)) return false;
    for (const char *p = text; *p; p++) {
        int tr, tc;
        char ch = *p == '_' ? ' ' : *p;
        if (!vn_engine_osk_locate(ch, &tr, &tc)) {
            fprintf(d->trace, "fail\tcharacter %c is not on the keyboard\n", ch);
            d->failed = true;
            return false;
        }
        for (int guard = 0; guard < 100; guard++) {
            int r, c;
            vn_engine_osk_cursor(d->e, &r, &c);
            if (r == tr && c == tc) break;
            uint32_t b = r < tr ? VN_DOWN : r > tr ? VN_UP : c < tc ? VN_RIGHT : VN_LEFT;
            if (!press(d, b)) return false;
        }
        if (!press(d, VN_CROSS)) return false;
    }
    return press(d, VN_START);
}

/* Performs one interaction step; returns false on failure or when nothing is left to do. */
static bool interact(drv *d)
{
    vn_mode m = vn_engine_mode(d->e);
    switch (m) {
    case MODE_MAIN_MENU:
        if (!move_cursor(d, 0)) return false;
        return press(d, VN_CROSS);
    case MODE_SAY: case MODE_PAUSE: {
        /* Press X until this interaction is over. Without --fast the text first types
         * out at the player's speed; a {w} click wait never completes, so after
         * 5 seconds the driver clicks anyway. */
        uint32_t start = vn_engine_interactions(d->e);
        for (int guard = 0; guard < 100; guard++) {
            if (vn_engine_interactions(d->e) != start || vn_engine_mode(d->e) != m) return true;
            if (!d->fast) {
                for (int i = 0; i < 300 && (m == MODE_PAUSE || !vn_engine_text_complete(d->e)); i++) {
                    if (vn_engine_interactions(d->e) != start || vn_engine_mode(d->e) != m) return true;
                    if (!step(d, 0, false)) return false;
                }
                if (vn_engine_interactions(d->e) != start || vn_engine_mode(d->e) != m) return true;
            }
            if (!press(d, VN_CROSS)) return false;
        }
        fprintf(d->trace, "fail\tthe interaction did not end after 100 presses of X\n");
        d->failed = true;
        return false;
    }
    case MODE_MENU: {
        if (d->choice_pos >= d->nchoices) {
            int count;
            vn_engine_cursor(d->e, &count);
            fprintf(d->trace, "need-choice\t%d\n", count);
            d->failed = true;
            return false;
        }
        int k = d->choices[d->choice_pos++];
        int count;
        vn_engine_cursor(d->e, &count);
        if (k < 0 || k >= count) {
            fprintf(d->trace, "fail\tchoice %d of %d\n", k, count);
            d->failed = true;
            return false;
        }
        return move_cursor(d, k) && press(d, VN_CROSS);
    }
    case MODE_INPUT:
        if (d->input_pos >= d->ninputs) {
            /* No scripted answer: accept the default. */
            return press(d, VN_START);
        }
        return type_text(d, d->inputs[d->input_pos++]);
    case MODE_TRANSITION: case MODE_RUN:
        if (d->fast) return press(d, VN_CROSS);
        return step(d, 0, false);
    case MODE_ERROR:
        fprintf(d->trace, "fail\tengine error: %s\n", vn_engine_error(d->e));
        d->failed = true;
        return false;
    default:
        fprintf(d->trace, "fail\tunexpected mode %s\n", mode_names[m]);
        d->failed = true;
        return false;
    }
}

static bool is_interaction(vn_mode m)
{
    return m == MODE_SAY || m == MODE_MENU || m == MODE_INPUT || m == MODE_PAUSE;
}

/* Runs until n interactions have been completed (or the story ends), then on
 * to the start of the next one, so `state` afterwards names a fresh checkpoint. */
static bool advance(drv *d, long n, bool until_end)
{
    long done = 0;
    while (!d->failed) {
        vn_mode m = vn_engine_mode(d->e);
        if (d->ended && m == MODE_MAIN_MENU) return true;
        bool enough = until_end ? n > 0 && done >= n : done >= n;
        if (enough && m != MODE_RUN && m != MODE_TRANSITION) return true;
        if (enough) {
            if (!step(d, 0, false)) return false;
            continue;
        }
        bool counted = is_interaction(m);
        if (!interact(d)) return false;
        if (counted) done++;
    }
    return false;
}

static uint64_t frame_hash(void)
{
    const uint32_t *px = gfx_soft_pixels();
    return vn_fnv1a64(px, sizeof(uint32_t) * GFX_W * GFX_H);
}

static bool open_menu_item(drv *d, int item)
{
    if (!press(d, VN_START)) return false;
    if (vn_engine_mode(d->e) != MODE_GAME_MENU) {
        fprintf(d->trace, "fail\tgame menu did not open\n");
        d->failed = true;
        return false;
    }
    return move_cursor(d, item) && press(d, VN_CROSS);
}

static bool goto_slot(drv *d, int slot)
{
    int page = (slot - 1) / 6, idx = (slot - 1) % 6;
    for (int i = 0; i < page; i++)
        if (!press(d, VN_R)) return false;
    int row = idx / 3, col = idx % 3;
    for (int i = 0; i < row; i++)
        if (!press(d, VN_DOWN)) return false;
    for (int i = 0; i < col; i++)
        if (!press(d, VN_RIGHT)) return false;
    return press(d, VN_CROSS);
}

static bool back_to_story(drv *d)
{
    for (int i = 0; i < 4; i++) {
        vn_mode m = vn_engine_mode(d->e);
        if (m != MODE_GAME_MENU && m != MODE_SAVE && m != MODE_LOAD && m != MODE_CONFIRM) return true;
        if (!press(d, VN_CIRCLE)) return false;
    }
    return true;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    return s;
}

static bool run_command(drv *d, char *line)
{
    char *cmd = strtok(line, " \t");
    if (!cmd) return true;
    char *arg = strtok(NULL, " \t");
    long n = arg ? strtol(arg, NULL, 10) : 1;
    if (strcmp(cmd, "choices") == 0) {
        for (char *a = arg; a && d->nchoices < (int)VN_ARRAY_LEN(d->choices); a = strtok(NULL, " \t"))
            d->choices[d->nchoices++] = atoi(a);
        return true;
    }
    if (strcmp(cmd, "inputs") == 0) {
        for (char *a = arg; a && d->ninputs < (int)VN_ARRAY_LEN(d->inputs); a = strtok(NULL, " \t"))
            d->inputs[d->ninputs++] = vn_strdup(a);
        return true;
    }
    if (strcmp(cmd, "play") == 0) return advance(d, arg ? n : 0, true);
    if (strcmp(cmd, "start") == 0) {
        if (vn_engine_mode(d->e) != MODE_MAIN_MENU) return true;
        d->ended = false;
        return move_cursor(d, 0) && press(d, VN_CROSS);
    }
    if (strcmp(cmd, "advance") == 0) return advance(d, n, false);
    if (strcmp(cmd, "wait") == 0) {
        for (long i = 0; i < n; i++)
            if (!step(d, 0, false)) return false;
        return true;
    }
    if (strcmp(cmd, "press") == 0 || strcmp(cmd, "hold") == 0) {
        uint32_t b = button(arg ? arg : "");
        char *cnt = strtok(NULL, " \t");
        long times = cnt ? strtol(cnt, NULL, 10) : 1;
        if (!b) {
            fprintf(d->trace, "fail\tunknown button %s\n", arg ? arg : "");
            return false;
        }
        for (long i = 0; i < times; i++) {
            if (cmd[0] == 'p' ? !press(d, b) : !step(d, b, false)) return false;
        }
        return cmd[0] == 'p' || step(d, 0, false);
    }
    if (strcmp(cmd, "hash") == 0) {
        vn_engine_render(d->e);
        fprintf(d->trace, "frame\t%016llx\n", (unsigned long long)frame_hash());
        return true;
    }
    if (strcmp(cmd, "state") == 0) {
        fprintf(d->trace, "state\t%016llx\n", (unsigned long long)vn_engine_state_hash(d->e));
        return true;
    }
    if (strcmp(cmd, "shot") == 0) {
        vn_engine_render(d->e);
        uint8_t *rgb = vn_alloc((size_t)GFX_W * GFX_H * 3);
        bool ok = arg && gfx_capture_rgb(rgb) && png_write_rgb(arg, rgb, GFX_W, GFX_H);
        free(rgb);
        if (!ok) fprintf(d->trace, "fail\tcannot write %s\n", arg ? arg : "(no file)");
        return ok;
    }
    if (strcmp(cmd, "save") == 0 || strcmp(cmd, "load") == 0) {
        if (n < 1 || n > 30) return false;
        bool save = cmd[0] == 's';
        vn_engine_render(d->e);   /* the thumbnail comes from the last frame */
        if (!open_menu_item(d, save ? 1 : 2) || !goto_slot(d, (int)n)) return false;
        if (vn_engine_mode(d->e) == MODE_CONFIRM) {
            if (!press(d, VN_UP) || !press(d, VN_CROSS)) return false;   /* "Yes" */
        }
        fprintf(d->trace, "%s\t%ld\n", cmd, n);
        return back_to_story(d);
    }
    if (strcmp(cmd, "qsave") == 0) {
        vn_engine_render(d->e);
        return press(d, VN_SELECT);
    }
    if (strcmp(cmd, "qload") == 0) {
        if (!open_menu_item(d, 4)) return false;
        if (vn_engine_mode(d->e) == MODE_CONFIRM && (!press(d, VN_UP) || !press(d, VN_CROSS))) return false;
        return back_to_story(d);
    }
    if (strcmp(cmd, "rollback") == 0) {
        for (long i = 0; i < n; i++) {
            /* Let the engine settle on an interaction before each step back. */
            for (int k = 0; k < 600 && !is_interaction(vn_engine_mode(d->e)); k++)
                if (!step(d, 0, false)) return false;
            if (!press(d, VN_L)) return false;
        }
        for (int k = 0; k < 600 && !is_interaction(vn_engine_mode(d->e)); k++)
            if (!step(d, 0, false)) return false;
        fprintf(d->trace, "rolled\t%ld\tdepth %d\n", n, vn_engine_rollback_depth(d->e));
        return true;
    }
    if (strcmp(cmd, "expect") == 0) {
        const char *want = arg ? arg : "";
        const char *have = mode_names[vn_engine_mode(d->e)];
        if (strcmp(want, have) != 0) {
            fprintf(d->trace, "fail\texpected mode %s, engine is in %s\n", want, have);
            d->failed = true;
            return false;
        }
        return true;
    }
    fprintf(d->trace, "fail\tunknown command %s\n", cmd);
    d->failed = true;
    return false;
}

static void write_wav_header(FILE *f, uint32_t frames)
{
    uint32_t data = frames * 4;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t v = 36 + data;
    memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16;
    memcpy(h + 16, &v, 4);
    uint16_t s = 1;
    memcpy(h + 20, &s, 2);
    s = 2;
    memcpy(h + 22, &s, 2);
    v = 44100;
    memcpy(h + 24, &v, 4);
    v = 44100 * 4;
    memcpy(h + 28, &v, 4);
    s = 4;
    memcpy(h + 32, &s, 2);
    s = 16;
    memcpy(h + 34, &s, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

int driver_main(const char *game, const char *save_dir, const char *script_path, const char *trace_path,
                const driver_opts *o)
{
    drv d;
    memset(&d, 0, sizeof d);
    d.max_frames = o->max_frames > 0 ? o->max_frames : 2000000;
    d.render_all = o->render_all;
    d.fast = o->fast;
    d.audio = o->audio || o->wav_path;
    d.audio_hash = 0xcbf29ce484222325ull;
    d.trace = trace_path ? fopen(trace_path, "w") : stdout;
    if (!d.trace) {
        fprintf(stderr, "vnpsp: cannot write %s\n", trace_path);
        return 2;
    }
    if (o->wav_path) {
        d.wav = fopen(o->wav_path, "wb");
        if (d.wav) fseek(d.wav, 44, SEEK_SET);
    }
    void *script;
    size_t size;
    if (!vn_fs_read_all(script_path, &script, &size)) {
        fprintf(stderr, "vnpsp: cannot read %s\n", script_path);
        return 2;
    }
    gfx_init();
    vn_config cfg = {game, save_dir, true, o->texture_budget, trace_line, &d};
    char err[256];
    d.e = vn_engine_new(&cfg, err, sizeof err);
    if (!d.e) {
        fprintf(stderr, "vnpsp: %s\n", err);
        fprintf(d.trace, "fail\t%s\n", err);
        free(script);
        return 1;
    }
    vn_engine_render(d.e);
    char *saveptr = (char *)script;
    for (char *line = saveptr; line && *line && !d.failed;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *t = trim(line);
        char *hash = strchr(t, '#');
        if (hash) *hash = 0;
        t = trim(t);
        if (*t && !run_command(&d, t)) d.failed = true;
        line = nl ? nl + 1 : NULL;
    }
    if (d.audio)
        fprintf(d.trace, "audio\t%016llx\t%llu\t%llu\n", (unsigned long long)d.audio_hash,
                (unsigned long long)d.audio_nonzero, (unsigned long long)d.audio_frames);
    uint32_t loads;
    uint64_t bytes;
    size_t cached;
    vn_engine_asset_stats(d.e, &loads, &bytes, &cached);
    fprintf(d.trace, "assets\t%u\t%llu\t%zu\n", (unsigned)loads, (unsigned long long)bytes, cached);
    fprintf(d.trace, "missing-glyphs\t%u\n", (unsigned)text_missing_glyphs());
    fprintf(d.trace, "frames\t%ld\n", d.frame);
    fprintf(d.trace, "%s\n", d.failed ? "result\tfail" : "result\tok");
    if (d.wav) {
        write_wav_header(d.wav, (uint32_t)d.audio_frames);
        fclose(d.wav);
    }
    vn_engine_free(d.e);
    gfx_shutdown();
    for (int i = 0; i < d.ninputs; i++) free(d.inputs[i]);
    free(script);
    if (d.trace != stdout) fclose(d.trace);
    return d.failed ? 1 : 0;
}
