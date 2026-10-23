/* Player-facing UI: the story's choice menu, the on-screen keyboard for
 * renpy.input, and the main, game, save/load, settings and history screens.
 * D-pad and analog nub navigate, X confirms, O goes back. */
#include "engine_internal.h"
#include "fs.h"
#include "gfx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FRAME_DT (1.0f / 60.0f)
#define OSK_COLS 16
#define OSK_ROWS 7   /* six rows of printable ASCII, then Delete / Done */
#define SLOTS_PER_PAGE 6

static const uint32_t WHITE = 0xFFFFFFFFu;

/* ---- navigation ------------------------------------------------------------------- */

static int nav(vn_engine *e, const vn_input *in, uint32_t neg, uint32_t pos, float stick)
{
    uint32_t dir = 0;
    if ((in->held & neg) || stick < -0.5f)
        dir = neg;
    else if ((in->held & pos) || stick > 0.5f)
        dir = pos;
    if (!dir) {
        if (e->nav_dir & (neg | pos)) e->nav_dir = 0;
        return 0;
    }
    int step = dir == neg ? -1 : 1;
    if ((in->pressed & dir) || e->nav_dir != dir) {
        e->nav_dir = dir;
        e->nav_t = 0.35f;
        return step;
    }
    e->nav_t -= FRAME_DT;
    if (e->nav_t <= 0) {
        e->nav_t = 0.08f;
        return step;
    }
    return 0;
}

int nav_vertical(vn_engine *e, const vn_input *in)
{
    return nav(e, in, VN_UP, VN_DOWN, in->stick_y);
}

int nav_horizontal(vn_engine *e, const vn_input *in)
{
    return nav(e, in, VN_LEFT, VN_RIGHT, in->stick_x);
}

static int wrap(int v, int n)
{
    return n ? ((v % n) + n) % n : 0;
}

static float centered_text(const vn_font *f, const char *s, float y, uint32_t color)
{
    float w = text_measure_plain(f, s);
    text_draw_plain(f, s, floorf((GFX_W - w) / 2), y, color);
    return w;
}

/* ---- choice menu ----------------------------------------------------------------- */

void choice_update(vn_engine *e, const vn_input *in)
{
    int d = nav_vertical(e, in);
    if (d) e->cursor = wrap(e->cursor + d, e->nitems);
    if (!(in->pressed & VN_CROSS) || e->nitems == 0) return;
    vn_buf b = {0};
    vn_buf_put(&b, "menu", 4);
    for (int i = 0; i <= e->nitems; i++) {
        bool picked = i == e->nitems;
        vn_buf_put(&b, picked ? "\t-> " : "\t", picked ? 4 : 1);
        eng_escape(&b, e->items[picked ? e->cursor : i].text);
    }
    vn_buf_u8(&b, 0);
    eng_trace(e, "%s", (const char *)b.data);
    vn_buf_free(&b);
    e->vm.pc = e->items[e->cursor].addr;
    e->vm.last_jump = e->vm.pc;
    e->st.caption = false;
    e->mode = MODE_RUN;
}

void choice_draw(vn_engine *e)
{
    const vn_font *f = eng_font(e, FONT_INTERFACE);
    text_layout tl[MENU_MAX];
    float total = 0, gap = 6;
    for (int i = 0; i < e->nitems; i++) {
        text_layout_init(&tl[i]);
        text_style ts = {&e->assets, f, WHITE, gfx_rgba(0, 0, 0, 255), 380, true, 22};
        text_layout_build(&tl[i], e->items[i].text, strlen(e->items[i].text), &ts);
        total += tl[i].height + 8 + gap;
    }
    float bottom = e->st.caption ? GFX_H - 90.0f : (float)GFX_H;
    float y = floorf((bottom - total) / 2);
    if (y < 4) y = 4;
    for (int i = 0; i < e->nitems; i++) {
        float h = tl[i].height + 8;
        bool sel = i == e->cursor;
        draw_panel(40, y, GFX_W - 80, h, sel ? gfx_rgba(60, 90, 160, 230) : gfx_rgba(0, 0, 0, 190),
                   sel ? gfx_rgba(255, 255, 255, 200) : gfx_rgba(255, 255, 255, 70));
        text_draw(&tl[i], 50, y + 4, tl[i].n, 1.0f, gfx_rgba(0, 0, 0, 255));
        y += h + gap;
        text_layout_free(&tl[i]);
    }
}

/* ---- on-screen keyboard -------------------------------------------------------------- */

bool vn_engine_osk_locate(char ch, int *row, int *col)
{
    if (ch < 32 || ch > 126) return false;
    int i = ch - 32;
    *row = i / OSK_COLS;
    *col = i % OSK_COLS;
    return true;
}

static void osk_finish(vn_engine *e)
{
    vm *v = &e->vm;
    value prompt = vm_peek(v, 2);
    vn_buf p = {0};
    v_to_str(prompt, &p);
    vn_buf_u8(&p, 0);
    vn_buf t = {0};
    eng_escape(&t, (const char *)p.data);
    vn_buf_u8(&t, '\t');
    eng_escape(&t, e->input_buf);
    vn_buf_u8(&t, 0);
    eng_trace(e, "input\t%s", (const char *)t.data);
    vn_buf_free(&p);
    vn_buf_free(&t);
    for (int i = 0; i < 3; i++) v_dec(vm_pop(v));
    vm_push(v, v_str(e->input_buf, (size_t)e->input_len));
    vm_ops o;
    if (vm_decode(v, &o)) v->pc += o.len;
    e->mode = MODE_RUN;
}

void osk_update(vn_engine *e, const vn_input *in)
{
    int dy = nav_vertical(e, in), dx = nav_horizontal(e, in);
    e->osk_row = wrap(e->osk_row + dy, OSK_ROWS);
    int cols = e->osk_row == OSK_ROWS - 1 ? 2 : OSK_COLS;
    if (dy && e->osk_row == OSK_ROWS - 1) e->osk_col = e->osk_col < OSK_COLS / 2 ? 0 : 1;
    if (dy && e->osk_row != OSK_ROWS - 1 && (dy > 0 ? e->osk_row == 0 : e->osk_row == OSK_ROWS - 2) &&
        e->osk_col <= 1)
        e->osk_col = e->osk_col * (OSK_COLS / 2);
    e->osk_col = wrap(e->osk_col + dx, cols);
    if (e->osk_row < OSK_ROWS - 1 && e->osk_row * OSK_COLS + e->osk_col > 126 - 32) e->osk_col = (126 - 32) % OSK_COLS;
    bool del = (in->pressed & VN_CIRCLE) != 0, done = (in->pressed & VN_START) != 0;
    if (in->pressed & VN_SQUARE && e->input_len < e->input_max) {
        e->input_buf[e->input_len++] = ' ';
        e->input_buf[e->input_len] = 0;
    }
    if (in->pressed & VN_CROSS) {
        if (e->osk_row == OSK_ROWS - 1) {
            if (e->osk_col == 0)
                del = true;
            else
                done = true;
        } else if (e->input_len < e->input_max) {
            e->input_buf[e->input_len++] = (char)(32 + e->osk_row * OSK_COLS + e->osk_col);
            e->input_buf[e->input_len] = 0;
        }
    }
    if (del && e->input_len > 0) e->input_buf[--e->input_len] = 0;
    if (done) osk_finish(e);
}

void osk_draw(vn_engine *e)
{
    const vn_font *f = eng_font(e, FONT_INTERFACE);
    draw_panel(16, 20, GFX_W - 32, GFX_H - 40, gfx_rgba(0, 0, 20, 220), gfx_rgba(255, 255, 255, 90));
    text_draw_plain(f, e->input_prompt, 28, 28, WHITE);
    float lh = (float)f->line_h;
    draw_panel(28, 30 + lh, GFX_W - 56, lh + 6, gfx_rgba(255, 255, 255, 40), gfx_rgba(255, 255, 255, 120));
    float tw = text_draw_plain(f, e->input_buf, 32, 33 + lh, WHITE);
    if (fmodf(e->blink, 1.0f) < 0.6f) gfx_fill_rect(33 + tw, 34 + lh, 1, lh - 2, WHITE);
    /* Seven rows plus the help line must fit inside the 232-pixel panel. */
    float cw = 26, ch = floorf((GFX_H - 40 - (44 + 2 * lh) - lh - 6) / OSK_ROWS), x0 = (GFX_W - cw * OSK_COLS) / 2,
          y0 = 40 + 2 * lh;
    for (int r = 0; r < OSK_ROWS - 1; r++) {
        for (int c = 0; c < OSK_COLS; c++) {
            int code = 32 + r * OSK_COLS + c;
            if (code > 126) continue;
            bool sel = r == e->osk_row && c == e->osk_col;
            float x = x0 + c * cw, y = y0 + r * ch;
            if (sel) draw_panel(x, y, cw - 2, ch - 2, gfx_rgba(60, 90, 160, 255), WHITE);
            char s[2] = {(char)code, 0};
            text_draw_plain(f, code == 32 ? "_" : s, x + (cw - text_measure_plain(f, code == 32 ? "_" : s)) / 2 - 1,
                            y + (ch - lh) / 2 - 1, WHITE);
        }
    }
    const char *labels[2] = {"Delete", "Done"};
    for (int c = 0; c < 2; c++) {
        bool sel = e->osk_row == OSK_ROWS - 1 && e->osk_col == c;
        float w = cw * OSK_COLS / 2, x = x0 + c * w, y = y0 + (OSK_ROWS - 1) * ch;
        draw_panel(x + 2, y, w - 4, ch - 2, sel ? gfx_rgba(60, 90, 160, 255) : gfx_rgba(255, 255, 255, 30),
                   sel ? WHITE : gfx_rgba(255, 255, 255, 80));
        text_draw_plain(f, labels[c], x + (w - text_measure_plain(f, labels[c])) / 2, y + (ch - lh) / 2 - 1, WHITE);
    }
    text_draw_plain(f, "X type   O delete   [] space   START done", 28, y0 + OSK_ROWS * ch + 2,
                    gfx_rgba(200, 200, 200, 255));
}

/* ---- screens ---------------------------------------------------------------------- */

enum { CONFIRM_OVERWRITE, CONFIRM_LOAD, CONFIRM_MAIN_MENU, CONFIRM_QUICK_LOAD };

static const char *const main_items[] = {"Start", "Load", "Settings", "Quit"};
static const char *const game_items[] = {"Return", "Save", "Load", "Quick save", "Quick load", "History",
                                        "Settings", "Main menu"};

static void refresh_slots(vn_engine *e)
{
    for (int i = 0; i < SAVE_SLOTS; i++) {
        char name[32];
        save_slot_name(i, name, sizeof name);
        save_read(e, name, false, &e->slots[i]);
        gfx_tex_destroy(e->slot_tex[i]);
        e->slot_tex[i] = NULL;
        vn_aligned_free(e->slot_rgba[i]);
        e->slot_rgba[i] = NULL;
        if (!e->slots[i].used || !e->slots[i].valid) continue;
        uint32_t *px = vn_aligned_alloc(sizeof(uint32_t) * THUMB_W * THUMB_H);
        for (int k = 0; k < THUMB_W * THUMB_H; k++) {
            uint16_t c = e->slots[i].thumb[k];
            px[k] = gfx_rgba((uint32_t)((c >> 11) << 3), (uint32_t)(((c >> 5) & 63) << 2), (uint32_t)((c & 31) << 3), 255);
        }
        gfx_tex_desc d = {GFX_8888, false, THUMB_W, THUMB_H, 7, 7, THUMB_W, px, sizeof(uint32_t) * THUMB_W * THUMB_H};
        e->slot_rgba[i] = px;
        e->slot_tex[i] = gfx_tex_create(&d);
    }
}

static bool story_mode(vn_mode m)
{
    return m == MODE_SAY || m == MODE_MENU || m == MODE_PAUSE || m == MODE_INPUT;
}

void ui_open(vn_engine *e, vn_mode m)
{
    if (e->mode == MODE_GAME_MENU) e->game_menu_cursor = e->ui_cursor;
    if (story_mode(e->mode)) e->overlay_return = e->mode;
    if (e->mode == MODE_MAIN_MENU) e->overlay_return = MODE_MAIN_MENU;
    e->ui_parent = e->mode;
    e->mode = m;
    e->ui_cursor = 0;
    e->ui_page = 0;
    if (m == MODE_SAVE || m == MODE_LOAD) refresh_slots(e);
    if (m == MODE_HISTORY) e->history_scroll = 0;
}

static void ui_back(vn_engine *e)
{
    if (e->mode == MODE_SETTINGS) settings_save(e);
    vn_mode to = e->mode == MODE_GAME_MENU ? e->overlay_return : e->ui_parent;
    e->ui_cursor = 0;
    if (to == MODE_GAME_MENU) {
        e->mode = MODE_GAME_MENU;
        e->ui_parent = e->overlay_return;
        e->ui_cursor = e->game_menu_cursor;
        return;
    }
    e->mode = to;
}

static void confirm(vn_engine *e, int action, int slot, const char *text)
{
    e->confirm_action = action;
    e->confirm_slot = slot;
    snprintf(e->confirm_text, sizeof e->confirm_text, "%s", text);
    vn_mode parent = e->mode;
    e->mode = MODE_CONFIRM;
    e->ui_parent = parent;
    e->ui_cursor = 1;   /* "No" by default */
}

static void load_slot(vn_engine *e, const char *name)
{
    if (save_read(e, name, true, NULL)) {
        eng_trace(e, "load\t%s", name);
        e->mode = MODE_RUN;
    }
}

static void do_confirm(vn_engine *e)
{
    char name[32];
    switch (e->confirm_action) {
    case CONFIRM_OVERWRITE:
        save_slot_name(e->confirm_slot, name, sizeof name);
        eng_message(e, save_write(e, name) ? "Saved." : "Saving failed.");
        refresh_slots(e);
        e->mode = MODE_SAVE;
        e->ui_parent = MODE_GAME_MENU;
        e->ui_cursor = e->confirm_slot % SLOTS_PER_PAGE;
        break;
    case CONFIRM_LOAD:
        save_slot_name(e->confirm_slot, name, sizeof name);
        load_slot(e, name);
        if (e->mode == MODE_CONFIRM) e->mode = MODE_LOAD;
        break;
    case CONFIRM_QUICK_LOAD:
        load_slot(e, "quick.sav");
        if (e->mode == MODE_CONFIRM) e->mode = MODE_GAME_MENU;
        break;
    case CONFIRM_MAIN_MENU:
        eng_to_main_menu(e);
        break;
    }
}

static void slots_update(vn_engine *e, const vn_input *in)
{
    int dx = nav_horizontal(e, in), dy = nav_vertical(e, in);
    int col = e->ui_cursor % 3, row = e->ui_cursor / 3;
    col += dx;
    if (col < 0) {
        col = 2;
        e->ui_page = wrap(e->ui_page - 1, SAVE_SLOTS / SLOTS_PER_PAGE);
    } else if (col > 2) {
        col = 0;
        e->ui_page = wrap(e->ui_page + 1, SAVE_SLOTS / SLOTS_PER_PAGE);
    }
    row = wrap(row + dy, 2);
    if (in->pressed & VN_L) e->ui_page = wrap(e->ui_page - 1, SAVE_SLOTS / SLOTS_PER_PAGE);
    if (in->pressed & VN_R) e->ui_page = wrap(e->ui_page + 1, SAVE_SLOTS / SLOTS_PER_PAGE);
    e->ui_cursor = row * 3 + col;
    int slot = e->ui_page * SLOTS_PER_PAGE + e->ui_cursor;
    if (!(in->pressed & VN_CROSS)) return;
    char name[32];
    save_slot_name(slot, name, sizeof name);
    if (e->mode == MODE_SAVE) {
        if (e->slots[slot].used) {
            confirm(e, CONFIRM_OVERWRITE, slot, "Overwrite this save?");
            return;
        }
        eng_message(e, save_write(e, name) ? "Saved." : "Saving failed.");
        if (e->mode == MODE_SAVE) refresh_slots(e);
    } else {
        if (!e->slots[slot].used) return;
        if (!e->slots[slot].valid) {
            eng_message(e, "This save is damaged or from a different build of the game.");
            return;
        }
        if (e->overlay_return != MODE_MAIN_MENU) {
            confirm(e, CONFIRM_LOAD, slot, "Load this save? Unsaved progress is lost.");
            return;
        }
        load_slot(e, name);
    }
}

static void settings_update(vn_engine *e, const vn_input *in)
{
    int dy = nav_vertical(e, in), dx = nav_horizontal(e, in);
    e->ui_cursor = wrap(e->ui_cursor + dy, 6);
    if (dx) {
        settings *s = &e->set;
        switch (e->ui_cursor) {
        case 0:
            if (s->cps == 0 && dx < 0) s->cps = 200;
            else if (s->cps == 0) break;
            else s->cps += 10.0f * dx;
            if (s->cps > 200) s->cps = 0;
            if (s->cps < 10 && s->cps != 0) s->cps = 10;
            break;
        case 1: s->auto_delay = vn_clampf(s->auto_delay + 0.5f * dx, 0.5f, 10.0f); break;
        case 2: s->vol_music = vn_clampf(roundf((s->vol_music + 0.1f * dx) * 10) / 10, 0, 1); break;
        case 3: s->vol_sfx = vn_clampf(roundf((s->vol_sfx + 0.1f * dx) * 10) / 10, 0, 1); break;
        case 4: s->vol_voice = vn_clampf(roundf((s->vol_voice + 0.1f * dx) * 10) / 10, 0, 1); break;
        }
        settings_apply(e);
    }
    if ((in->pressed & VN_CROSS) && e->ui_cursor == 5) ui_back(e);
}

void ui_update(vn_engine *e, const vn_input *in)
{
    bool back = (in->pressed & VN_CIRCLE) != 0;
    switch (e->mode) {
    case MODE_MAIN_MENU: {
        e->ui_cursor = wrap(e->ui_cursor + nav_vertical(e, in), 4);
        if (!(in->pressed & VN_CROSS)) return;
        switch (e->ui_cursor) {
        case 0: eng_new_game(e); break;
        case 1: ui_open(e, MODE_LOAD); break;
        case 2: ui_open(e, MODE_SETTINGS); break;
        case 3: e->mode = MODE_QUIT; break;
        }
        return;
    }
    case MODE_GAME_MENU: {
        if (back || (in->pressed & VN_START)) {
            ui_back(e);
            return;
        }
        e->ui_cursor = wrap(e->ui_cursor + nav_vertical(e, in), (int)VN_ARRAY_LEN(game_items));
        if (!(in->pressed & VN_CROSS)) return;
        switch (e->ui_cursor) {
        case 0: ui_back(e); break;
        case 1: ui_open(e, MODE_SAVE); break;
        case 2: ui_open(e, MODE_LOAD); break;
        case 3:
            eng_message(e, save_write(e, "quick.sav") ? "Quick saved." : "Quick save failed.");
            break;
        case 4: {
            struct slot_info info;
            if (!save_read(e, "quick.sav", false, &info) || !info.valid) {
                eng_message(e, "There is no quick save to load.");
                break;
            }
            confirm(e, CONFIRM_QUICK_LOAD, -1, "Load the quick save? Unsaved progress is lost.");
            break;
        }
        case 5: ui_open(e, MODE_HISTORY); break;
        case 6: ui_open(e, MODE_SETTINGS); break;
        case 7: confirm(e, CONFIRM_MAIN_MENU, 0, "Return to the main menu? Unsaved progress is lost."); break;
        }
        return;
    }
    case MODE_SAVE: case MODE_LOAD:
        if (back) {
            ui_back(e);
            return;
        }
        slots_update(e, in);
        return;
    case MODE_SETTINGS:
        if (back) {
            ui_back(e);
            return;
        }
        settings_update(e, in);
        return;
    case MODE_HISTORY: {
        if (back || (in->pressed & VN_TRIANGLE)) {
            ui_back(e);
            return;
        }
        int d = nav_vertical(e, in);
        e->history_scroll -= d;
        if (e->history_scroll < 0) e->history_scroll = 0;
        if (e->history_scroll > e->history_n - 1) e->history_scroll = e->history_n > 0 ? e->history_n - 1 : 0;
        return;
    }
    case MODE_CONFIRM: {
        int d = nav_horizontal(e, in) + nav_vertical(e, in);
        if (d) e->ui_cursor = wrap(e->ui_cursor + d, 2);
        if (back || ((in->pressed & VN_CROSS) && e->ui_cursor == 1)) {
            e->mode = e->ui_parent;
            if (e->mode == MODE_SAVE || e->mode == MODE_LOAD) e->ui_parent = e->overlay_return == MODE_MAIN_MENU ? MODE_MAIN_MENU : MODE_GAME_MENU;
            if (e->mode == MODE_GAME_MENU) e->ui_parent = e->overlay_return;
            e->ui_cursor = 0;
            return;
        }
        if (in->pressed & VN_CROSS) do_confirm(e);
        return;
    }
    default:
        return;
    }
}

static void draw_list(vn_engine *e, const char *const *items, int n, float y)
{
    const vn_font *f = eng_font(e, FONT_INTERFACE);
    float h = (float)f->line_h + 8;
    for (int i = 0; i < n; i++) {
        bool sel = i == e->ui_cursor;
        if (sel) draw_panel(150, y + i * h, GFX_W - 300, h - 2, gfx_rgba(60, 90, 160, 230), gfx_rgba(255, 255, 255, 180));
        centered_text(f, items[i], y + i * h + 4, sel ? WHITE : gfx_rgba(210, 210, 210, 255));
    }
}

static void format_time(uint64_t t, char *out, size_t cap)
{
    if (t == 0) {
        snprintf(out, cap, "-");
        return;
    }
    time_t tt = (time_t)t;
    struct tm *tm = localtime(&tt);
    if (!tm || !strftime(out, cap, "%Y-%m-%d %H:%M", tm)) snprintf(out, cap, "-");
}

static void draw_slots(vn_engine *e)
{
    const vn_font *f = eng_font(e, FONT_INTERFACE);
    char title[64];
    snprintf(title, sizeof title, "%s - page %d/%d   (L/R: page)", e->mode == MODE_SAVE ? "Save" : "Load", e->ui_page + 1,
             SAVE_SLOTS / SLOTS_PER_PAGE);
    centered_text(f, title, 6, WHITE);
    float cw = 150, chh = 116, x0 = (GFX_W - 3 * cw) / 2, y0 = 26;
    for (int i = 0; i < SLOTS_PER_PAGE; i++) {
        int slot = e->ui_page * SLOTS_PER_PAGE + i;
        float x = x0 + (i % 3) * cw, y = y0 + (i / 3) * (chh + 4);
        bool sel = i == e->ui_cursor;
        draw_panel(x + 2, y, cw - 4, chh, sel ? gfx_rgba(60, 90, 160, 230) : gfx_rgba(0, 0, 0, 200),
                   sel ? WHITE : gfx_rgba(255, 255, 255, 70));
        float tx = x + (cw - THUMB_W) / 2, ty = y + 4;
        if (e->slot_tex[slot]) {
            float xy[8] = {tx, ty, tx + THUMB_W, ty, tx + THUMB_W, ty + THUMB_H, tx, ty + THUMB_H};
            float uv[8] = {0, 0, THUMB_W, 0, THUMB_W, THUMB_H, 0, THUMB_H};
            gfx_draw_quad(e->slot_tex[slot], xy, uv, WHITE, GFX_NEAREST, NULL);
        } else {
            gfx_fill_rect(tx, ty, THUMB_W, THUMB_H, gfx_rgba(40, 40, 40, 255));
        }
        char label[96], when[32];
        const struct slot_info *si = &e->slots[slot];
        if (!si->used)
            snprintf(label, sizeof label, "%d. empty", slot + 1);
        else if (!si->valid)
            snprintf(label, sizeof label, "%d. unusable", slot + 1);
        else {
            format_time(si->time, when, sizeof when);
            snprintf(label, sizeof label, "%d. %s", slot + 1, when);
        }
        text_draw_plain(f, label, x + 8, ty + THUMB_H + 2, WHITE);
        if (si->valid && si->desc[0]) {
            char d[24];
            size_t n = strlen(si->desc) < 20 ? strlen(si->desc) : 20;
            while (n && (si->desc[n] & 0xC0) == 0x80) n--;
            memcpy(d, si->desc, n);
            d[n] = 0;
            text_draw_plain(f, d, x + 8, ty + THUMB_H + 2 + f->line_h, gfx_rgba(190, 190, 190, 255));
        }
    }
}

static void draw_settings(vn_engine *e)
{
    const vn_font *f = eng_font(e, FONT_INTERFACE);
    centered_text(f, "Settings", 20, WHITE);
    char lines[6][64];
    if (e->set.cps == 0)
        snprintf(lines[0], 64, "Text speed: instant");
    else
        snprintf(lines[0], 64, "Text speed: %d characters/s", (int)e->set.cps);
    snprintf(lines[1], 64, "Auto-forward delay: %.1f s", e->set.auto_delay);
    snprintf(lines[2], 64, "Music volume: %d%%", (int)(e->set.vol_music * 100 + 0.5f));
    snprintf(lines[3], 64, "Sound volume: %d%%", (int)(e->set.vol_sfx * 100 + 0.5f));
    snprintf(lines[4], 64, "Voice volume: %d%%", (int)(e->set.vol_voice * 100 + 0.5f));
    snprintf(lines[5], 64, "Back");
    const char *items[6];
    for (int i = 0; i < 6; i++) items[i] = lines[i];
    draw_list(e, items, 6, 60);
    centered_text(f, "Left/Right: change   O: back", GFX_H - 24, gfx_rgba(190, 190, 190, 255));
}

static void draw_history(vn_engine *e)
{
    const vn_font *f = eng_font(e, FONT_TEXT), *nf = eng_font(e, FONT_NAME);
    draw_panel(8, 8, GFX_W - 16, GFX_H - 16, gfx_rgba(0, 0, 0, 220), gfx_rgba(255, 255, 255, 80));
    float y = GFX_H - 16;
    text_style ts = {&e->assets, f, WHITE, gfx_rgba(0, 0, 0, 255), GFX_W - 48, false, 22};
    for (int i = e->history_n - 1 - e->history_scroll; i >= 0 && y > 16; i--) {
        text_layout tl;
        text_layout_init(&tl);
        vn_buf b = {0};
        for (const char *p = e->history[i].text; *p; p++) {
            vn_buf_u8(&b, (uint8_t)*p);
            if (*p == '{') vn_buf_u8(&b, '{');
        }
        text_layout_build(&tl, (const char *)b.data, b.len, &ts);
        vn_buf_free(&b);
        float h = tl.height + (e->history[i].who ? nf->line_h : 0) + 6;
        y -= h;
        gfx_set_clip(8, 8, GFX_W - 16, GFX_H - 16);
        if (e->history[i].who) text_draw_plain(nf, e->history[i].who, 24, y, gfx_rgba(255, 220, 150, 255));
        text_draw(&tl, 24, y + (e->history[i].who ? nf->line_h : 0), tl.n, 1.0f, 0);
        gfx_reset_clip();
        text_layout_free(&tl);
    }
}

void ui_draw(vn_engine *e)
{
    const vn_font *f = eng_font(e, FONT_INTERFACE);
    if (e->mode == MODE_MAIN_MENU || e->overlay_return == MODE_MAIN_MENU) {
        text_layout tl;
        text_layout_init(&tl);
        vn_buf b = {0};
        vn_buf_put(&b, "{size=*1.6}", 11);
        for (const char *p = vn_engine_title(e); *p; p++) {
            vn_buf_u8(&b, (uint8_t)*p);
            if (*p == '{') vn_buf_u8(&b, '{');
        }
        text_style ts = {&e->assets, f, WHITE, 0, GFX_W - 40, true, 22};
        text_layout_build(&tl, (const char *)b.data, b.len, &ts);
        vn_buf_free(&b);
        if (e->mode == MODE_MAIN_MENU) text_draw(&tl, 20, 50, tl.n, 1.0f, 0);
        text_layout_free(&tl);
    } else {
        gfx_fill_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0, 0, 0, 150));
    }
    switch (e->mode) {
    case MODE_MAIN_MENU: draw_list(e, main_items, 4, 130); break;
    case MODE_GAME_MENU: draw_list(e, game_items, (int)VN_ARRAY_LEN(game_items), 40); break;
    case MODE_SAVE: case MODE_LOAD: draw_slots(e); break;
    case MODE_SETTINGS: draw_settings(e); break;
    case MODE_HISTORY: draw_history(e); break;
    case MODE_CONFIRM: {
        draw_panel(60, 90, GFX_W - 120, 90, gfx_rgba(0, 0, 0, 230), WHITE);
        text_layout tl;
        text_layout_init(&tl);
        text_style ts = {&e->assets, f, WHITE, 0, GFX_W - 140, true, 22};
        text_layout_build(&tl, e->confirm_text, strlen(e->confirm_text), &ts);
        text_draw(&tl, 70, 100, tl.n, 1.0f, 0);
        text_layout_free(&tl);
        const char *opts[2] = {"Yes", "No"};
        for (int i = 0; i < 2; i++) {
            float x = 140.0f + 120.0f * (float)i;
            bool sel = e->ui_cursor == i;
            draw_panel(x, 145, 80, (float)f->line_h + 8, sel ? gfx_rgba(60, 90, 160, 255) : gfx_rgba(255, 255, 255, 30),
                       sel ? WHITE : gfx_rgba(255, 255, 255, 80));
            text_draw_plain(f, opts[i], x + (80 - text_measure_plain(f, opts[i])) / 2, 149, WHITE);
        }
        break;
    }
    default: break;
    }
}
