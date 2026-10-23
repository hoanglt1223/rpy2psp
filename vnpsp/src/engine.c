#include "engine_internal.h"
#include "fs.h"
#include "gfx.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOX_MARGIN 6.0f
#define BOX_PAD 8.0f
#define BOX_MIN_LINES 3

/* ---- small helpers --------------------------------------------------------------- */

void eng_escape(vn_buf *b, const char *s)
{
    for (; *s; s++) {
        if (*s == '\n')
            vn_buf_put(b, "\\n", 2);
        else if (*s == '\t')
            vn_buf_put(b, "\\t", 2);
        else if (*s == '\\')
            vn_buf_put(b, "\\\\", 2);
        else
            vn_buf_u8(b, (uint8_t)*s);
    }
}

void eng_trace(vn_engine *e, const char *fmt, ...)
{
    if (!e->cfg.trace) return;
    char line[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    e->cfg.trace(e->cfg.trace_user, line);
}

/* Trace fields go through this so text containing tabs or newlines stays on one line. */
static const char *esc(vn_buf *b, const char *s)
{
    b->len = 0;
    eng_escape(b, s ? s : "None");
    vn_buf_u8(b, 0);
    return (const char *)b->data;
}

void eng_fail(vn_engine *e, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->err, sizeof e->err, fmt, ap);
    va_end(ap);
    vn_log("vnpsp: %s", e->err);
    eng_trace(e, "error\t%s", e->err);
    e->mode = MODE_ERROR;
}

void eng_message(vn_engine *e, const char *msg)
{
    snprintf(e->message, sizeof e->message, "%s", msg);
    e->message_t = 3.0f;
    eng_trace(e, "message\t%s", msg);
}

const vn_font *eng_font(vn_engine *e, int role)
{
    return vn_assets_font(&e->assets, role);
}

static bool lookup_var(void *user, const char *name, size_t len, value *out)
{
    vn_engine *e = user;
    int32_t sym = vm_find_sym(&e->vm, name, len);
    if (sym < 0) return false;
    return vm_load(&e->vm, (uint16_t)sym, out);
}

/* Interpolates s; on failure reports the error and returns NULL. */
static char *interpolate(vn_engine *e, const char *s)
{
    vn_buf b = {0};
    verr err;
    if (!text_interpolate(s, strlen(s), lookup_var, e, &b, &err)) {
        vn_buf_free(&b);
        eng_fail(e, "%s (in \"%.80s\")", err.msg, s);
        return NULL;
    }
    vn_buf_u8(&b, 0);
    return (char *)b.data;
}

static uint32_t char_color(const uint8_t c[4], uint32_t dflt)
{
    return c[3] ? gfx_rgba(c[0], c[1], c[2], c[3]) : dflt;
}

/* ---- audio state ---------------------------------------------------------------- */

static void start_channel(vn_engine *e, int ch, const chan_state *cs, bool queue, float fadein, float fadeout, float volume)
{
    vn_source *srcs[MIX_PLAYLIST_MAX];
    int n = 0;
    for (int i = 0; i < cs->n; i++) {
        uint8_t *data;
        uint32_t size;
        if (!vn_assets_read(&e->assets, cs->assets[i], &data, &size)) {
            vn_log("audio: asset %s is missing from the pack", vn_str(&e->script, e->script.assets[cs->assets[i]].key));
            continue;
        }
        vn_source *s = source_new(data, size);
        if (s) srcs[n++] = s;
    }
    if (n) mixer_play(e->mixer, ch, srcs, n, cs->loop, queue, fadein, fadeout, volume);
}

void eng_apply_audio_state(vn_engine *e, const chan_state *before)
{
    for (uint32_t i = 0; i < e->script.nchannels && i < MIX_MAX_CHANNELS; i++) {
        const chan_state *a = &before[i], *b = &e->st.chans[i];
        bool same = a->n == b->n && a->loop == b->loop && memcmp(a->assets, b->assets, sizeof(uint16_t) * a->n) == 0;
        /* Only looping tracks are restarted; a one-shot sound is not replayed. */
        if (!same) {
            if (b->n && b->loop)
                start_channel(e, (int)i, b, false, 0, 0, 1.0f);
            else
                mixer_stop(e->mixer, (int)i, 0);
        }
        if (a->volume != b->volume) mixer_channel_volume(e->mixer, (int)i, b->volume, 0);
    }
}

static void stop_all_audio(vn_engine *e)
{
    for (uint32_t i = 0; i < e->script.nchannels && i < MIX_MAX_CHANNELS; i++) {
        mixer_stop(e->mixer, (int)i, 0);
        mixer_channel_volume(e->mixer, (int)i, 1.0f, 0);
        memset(&e->st.chans[i], 0, sizeof e->st.chans[i]);
        e->st.chans[i].volume = 1.0f;
    }
}

/* ---- story reset ------------------------------------------------------------------ */

static void story_reset(vn_engine *e)
{
    story *st = &e->st;
    free(st->last_text);
    free(st->caption_text);
    for (uint32_t i = 0; i < st->nnvl; i++) {
        free(st->nvl[i].who);
        free(st->nvl[i].text);
    }
    free(st->nvl);
    scene_clear(&st->sc);
    scene_clear(&e->committed);
    chan_state chans[MIX_MAX_CHANNELS];
    memcpy(chans, st->chans, sizeof chans);
    scene keep = st->sc;
    memset(st, 0, sizeof *st);
    st->sc = keep;
    memcpy(st->chans, chans, sizeof chans);
    st->window = WINDOW_AUTO;
    st->revert_image = VN_NONE16;
    e->sctx.clock = 0;
    e->trans = NULL;
    e->snapshot_pending = false;
    e->say_trans_done = false;
    e->voice_fresh = false;
    e->auto_mode = false;
    e->box_hidden = false;
    text_layout_free(&e->say);
    free(e->say_who);
    e->say_who = NULL;
    for (int i = 0; i < e->nitems; i++) free(e->items[i].text);
    e->nitems = 0;
}

void eng_to_main_menu(vn_engine *e)
{
    stop_all_audio(e);
    story_reset(e);
    vm_reset(&e->vm);
    rb_free(e);
    history_free(e);
    persistent_save(e, false);
    e->mode = MODE_MAIN_MENU;
    e->ui_cursor = 0;
}

/* Loads a scene's images when the story enters its label, and reports the bytes read. */
static void prefetch(vn_engine *e, uint32_t addr)
{
    uint64_t bytes = vn_assets_prefetch_label(&e->assets, addr);
    const char *label = vn_script_label_at(&e->script, addr);
    if (bytes && label) eng_trace(e, "prefetch\t%s\t%llu", label, (unsigned long long)bytes);
}

void eng_new_game(vn_engine *e)
{
    stop_all_audio(e);
    story_reset(e);
    vm_reset(&e->vm);
    rb_free(e);
    history_free(e);
    if (!e->cfg.deterministic) vm_rng_seed(&e->vm.rng, (uint32_t)(e->frame * 2654435761u) ^ 0x5eed);
    if (!vm_run_routine(&e->vm, e->script.defaults_addr)) {
        eng_fail(e, "defaults: %s", e->vm.err.msg);
        return;
    }
    vm_start(&e->vm, e->script.start_addr);
    prefetch(e, e->script.start_addr);
    e->mode = MODE_RUN;
}

/* ---- say ------------------------------------------------------------------------- */

static float base_text_size(const vn_engine *e)
{
    return e->script.src_h * 22.0f / 720.0f;
}

static const vn_char *speaker(vn_engine *e)
{
    return &e->script.chars[e->say_char < e->script.nchars ? e->say_char : 0];
}

/* The speaker's side image: show_side_image, or Ren'Py's automatic one, the image
 * tagged "side" that names the character's image tag and best matches the
 * attributes that tag is shown with. */
static uint16_t side_disp(vn_engine *e, const vn_char *c)
{
    if (c->side_image != VN_NONE16) return c->side_image;
    if (c->image == VN_NONE32) return VN_NONE16;
    const vn_script *s = &e->script;
    const char *tag = vn_str(s, c->image);
    sprite *sp = scene_find(&e->st.sc, tag);
    const vn_image *shown = sp && sp->image < s->nimages ? &s->images[sp->image] : NULL;
    uint16_t best = VN_NONE16;
    int best_score = -1;
    for (uint32_t i = 0; i < s->nimages; i++) {
        const vn_image *im = &s->images[i];
        if (strcmp(im->tag, "side") != 0 || im->disp == VN_NONE16) continue;
        bool names_tag = false, ok = true;
        int score = 0;
        for (int k = 0; k < im->nattrs && ok; k++) {
            if (!names_tag && strcmp(im->attrs[k], tag) == 0) {
                names_tag = true;
                continue;
            }
            bool found = false;
            for (int j = 0; shown && j < shown->nattrs; j++) found |= strcmp(im->attrs[k], shown->attrs[j]) == 0;
            if (found)
                score++;
            else if (shown)
                ok = false;
            else
                score--;   /* tag not on screen: prefer the plainest side image */
        }
        if (names_tag && ok && score > best_score) {
            best_score = score;
            best = im->disp;
        }
    }
    return best;
}

static float side_width(vn_engine *e, const vn_char *c)
{
    uint16_t disp = side_disp(e, c);
    if (disp == VN_NONE16) return 0;
    const vn_disp *d = &e->script.disps[disp];
    if (d->kind != DISP_TEXTURE || d->asset == VN_NONE16) return 0;
    vn_texture *t = vn_assets_texture(&e->assets, d->asset);
    if (!t) return 0;
    vn_texture_bounds(t);
    float w = t->opaque_x1;
    return w > 200 ? 200 : w;
}

static float say_width(vn_engine *e)
{
    const vn_char *c = speaker(e);
    if (c->kind == 1) return GFX_W - 32.0f;
    if (c->flags & CHAR_CENTERED) return 400.0f;
    float side = side_width(e, c);
    return GFX_W - 2 * BOX_MARGIN - 2 * BOX_PAD - (side > 0 ? side + 4 : 0);
}

static void layout_say(vn_engine *e, const char *text, uint32_t color)
{
    const vn_char *c = speaker(e);
    text_style ts = {&e->assets, eng_font(e, FONT_TEXT), color, gfx_rgba(0, 0, 0, 255), say_width(e),
                     (c->flags & CHAR_CENTERED) != 0, base_text_size(e)};
    text_layout_build(&e->say, text, strlen(text), &ts);
}

static uint32_t next_wait(const vn_engine *e)
{
    for (uint32_t i = e->ev_next; i < e->say.nev; i++)
        if (e->say.ev[i].kind == TEV_WAIT) return e->say.ev[i].at;
    return e->say.n;
}

/* Applies text events the reveal has reached; returns true while a wait holds it. */
static bool process_events(vn_engine *e)
{
    while (e->ev_next < e->say.nev && e->say.ev[e->ev_next].at <= (uint32_t)e->reveal) {
        const tevent *ev = &e->say.ev[e->ev_next];
        if (ev->kind == TEV_CPS) {
            if (ev->arg == -1.0f)
                e->cps_now = e->set.cps;
            else if (ev->arg <= -2.0f)
                e->cps_now = e->set.cps * -(ev->arg + 2.0f);
            else
                e->cps_now = ev->arg;
        } else if (ev->kind == TEV_WAIT) {
            if (e->wait_until != ev->at) {
                e->wait_until = ev->at;
                e->wait_timer = ev->arg;
                e->reveal = (float)ev->at;
                return true;
            }
            if (e->wait_timer != 0.0f) return true;
        }
        e->ev_next++;
        e->wait_until = UINT32_MAX;
    }
    return false;
}

static void say_reveal(vn_engine *e, float dt)
{
    if (e->say_done) return;
    if (e->wait_until != UINT32_MAX) {
        if (e->wait_timer > 0) {
            e->wait_timer -= dt;
            if (e->wait_timer <= 0) {
                e->wait_timer = 0;
                e->wait_until = UINT32_MAX;
                e->ev_next++;
            }
        }
        if (e->wait_until != UINT32_MAX) return;
    }
    if (process_events(e)) return;
    float stop = (float)next_wait(e);
    if (e->cps_now <= 0 || e->skip)
        e->reveal = stop;
    else
        e->reveal = fminf(stop, e->reveal + e->cps_now * dt);
    if (process_events(e)) return;
    if (e->reveal >= (float)e->say.n && e->ev_next >= e->say.nev) e->say_done = true;
}

/* Click: release a {w}, show the rest up to the next wait, or report that the line can end. */
static bool say_click(vn_engine *e)
{
    if (e->say_done) return true;
    if (e->wait_until != UINT32_MAX) {
        e->wait_until = UINT32_MAX;
        e->wait_timer = 0;
        e->ev_next++;
    }
    e->reveal = (float)next_wait(e);
    while (e->ev_next < e->say.nev && e->say.ev[e->ev_next].at <= (uint32_t)e->reveal &&
           e->say.ev[e->ev_next].kind != TEV_WAIT)
        e->ev_next++;
    if (e->reveal >= (float)e->say.n && e->ev_next >= e->say.nev) e->say_done = true;
    return false;
}

static void end_line(vn_engine *e)
{
    story *st = &e->st;
    if (st->revert_pending) {
        sprite *sp = scene_find(&st->sc, st->revert_key);
        if (sp && st->revert_image < e->script.nimages && e->script.images[st->revert_image].disp != VN_NONE16) {
            sp->image = st->revert_image;
            sp->disp = e->script.images[st->revert_image].disp;
        }
        st->revert_pending = false;
    }
    e->mode = MODE_RUN;
}

static void say_update(vn_engine *e, const vn_input *in, float dt)
{
    say_reveal(e, dt);
    bool advance = false;
    if (in->pressed & VN_CROSS) {
        if (e->box_hidden)
            e->box_hidden = false;
        else
            advance = say_click(e);
    }
    if (e->say_done) {
        if (e->skip || e->say.no_wait) advance = true;
        if (e->auto_mode) {
            e->auto_timer += dt;
            float need = e->set.auto_delay * (0.5f + (float)e->say.n / 120.0f);
            if (e->auto_timer >= need) advance = true;
        }
    }
    if (advance) end_line(e);
}

/* Shows a line. extend: continue the previous line's text. */
static bool start_say(vn_engine *e, uint16_t ci, const char *raw, bool interact)
{
    const vn_script *s = &e->script;
    story *st = &e->st;
    const vn_char *c = &s->chars[ci];
    bool extend = (c->flags & CHAR_EXTEND) != 0;
    uint16_t who_ci = extend ? st->last_speaker : ci;
    const vn_char *who = &s->chars[who_ci];

    char *added = interpolate(e, raw);
    if (!added) return false;
    /* last_text keeps the line without what_prefix/what_suffix so extend can add inside them. */
    vn_buf body = {0};
    if (extend && st->last_text) vn_buf_put(&body, st->last_text, strlen(st->last_text));
    vn_buf_put(&body, added, strlen(added));
    vn_buf_u8(&body, 0);
    free(added);
    const char *pre = vn_str(s, who->what_prefix), *suf = vn_str(s, who->what_suffix);
    vn_buf full = {0};
    vn_buf_put(&full, pre, strlen(pre));
    vn_buf_put(&full, body.data, body.len - 1);
    vn_buf_put(&full, suf, strlen(suf));
    vn_buf_u8(&full, 0);

    char *name = NULL;
    if (who->name != VN_NONE32) {
        char *n = interpolate(e, vn_str(s, who->name));
        if (!n) {
            vn_buf_free(&full);
            vn_buf_free(&body);
            return false;
        }
        vn_buf nb = {0};
        const char *wpre = vn_str(s, who->who_prefix), *wsuf = vn_str(s, who->who_suffix);
        vn_buf_put(&nb, wpre, strlen(wpre));
        vn_buf_put(&nb, n, strlen(n));
        vn_buf_put(&nb, wsuf, strlen(wsuf));
        vn_buf_u8(&nb, 0);
        free(n);
        name = (char *)nb.data;
    }

    uint32_t prev_count = 0;
    if (extend && st->last_text) {
        /* Continue typing where the previous line ended, before its suffix. */
        uint32_t suffix_chars = 0;
        for (const char *q = suf; *q; q++) suffix_chars += ((uint8_t)*q & 0xC0) != 0x80;
        prev_count = (uint32_t)e->reveal > suffix_chars ? (uint32_t)e->reveal - suffix_chars : 0;
    }
    free(st->last_text);
    st->last_text = (char *)body.data;
    st->last_speaker = who_ci;

    if (!interact) {
        st->caption = true;
        st->caption_char = who_ci;
        free(st->caption_text);
        st->caption_text = vn_strdup((const char *)full.data);
        free(name);
        vn_buf_free(&full);
        return true;
    }

    {
        vn_buf tb = {0};
        eng_escape(&tb, (const char *)full.data);
        vn_buf_u8(&tb, 0);
        eng_trace(e, "text\t%s", (const char *)tb.data);
        vn_buf_free(&tb);
    }
    e->say_char = who_ci;
    free(e->say_who);
    e->say_who = name;
    e->say_who_color = char_color(who->who_color, gfx_rgba(255, 255, 255, 255));
    layout_say(e, (const char *)full.data, char_color(who->what_color, gfx_rgba(255, 255, 255, 255)));
    e->cps_now = e->set.cps;
    e->reveal = (float)prev_count;
    e->ev_next = 0;
    while (e->ev_next < e->say.nev && e->say.ev[e->ev_next].at < prev_count) e->ev_next++;
    for (uint32_t i = 0; i < e->say.nev; i++)
        if (e->say.ev[i].kind == TEV_FAST && (float)e->say.ev[i].at > e->reveal) {
            e->reveal = (float)e->say.ev[i].at;
            while (e->ev_next < e->say.nev && e->say.ev[e->ev_next].at <= e->say.ev[i].at &&
                   e->say.ev[e->ev_next].kind != TEV_WAIT)
                e->ev_next++;
        }
    e->wait_until = UINT32_MAX;
    e->wait_timer = 0;
    e->say_done = false;
    e->auto_timer = 0;
    e->box_hidden = false;

    vn_buf plain = {0};
    text_plain((const char *)full.data, strlen((const char *)full.data), &plain);
    vn_buf_u8(&plain, 0);
    if (extend && e->history_n) {
        free(e->history[e->history_n - 1].text);
        e->history[e->history_n - 1].text = vn_strdup((const char *)plain.data);
    } else {
        history_add(e, name, (const char *)plain.data);
    }
    if (who->kind == 1) {
        st->nvl = vn_realloc(st->nvl, sizeof(nvl_line) * (st->nnvl + 1));
        if (extend && st->nnvl) {
            st->nnvl--;
            free(st->nvl[st->nnvl].who);
            free(st->nvl[st->nnvl].text);
        }
        nvl_line *l = &st->nvl[st->nnvl++];
        l->who = name ? vn_strdup(name) : NULL;
        l->text = vn_strdup((const char *)full.data);
        l->color = e->say_who_color;
    }
    vn_buf_free(&plain);
    vn_buf_free(&full);
    st->caption = false;
    e->mode = MODE_SAY;
    return true;
}

/* ---- host instructions ----------------------------------------------------------- */

static void trace_show(vn_engine *e, const char *kind, const vn_show *rec)
{
    vn_buf b = {0}, t = {0};
    if (!rec) {
        eng_trace(e, "%s\tNone", kind);
        return;
    }
    for (int i = 0; i < rec->nattrs; i++) {
        if (i) vn_buf_u8(&b, ' ');
        const char *a = vn_str(&e->script, vn_le32(rec->attrs + 4 * i));
        vn_buf_put(&b, a, strlen(a));
    }
    vn_buf_u8(&b, 0);
    eng_trace(e, "%s\t%s\t%s", kind, esc(&t, vn_str(&e->script, rec->tag)), (const char *)b.data);
    vn_buf_free(&b);
    vn_buf_free(&t);
}

static void voice_line_start(vn_engine *e)
{
    /* A voice plays for the line after it; without a new one the old stops, unless sustained. */
    if (!e->voice_fresh && !e->st.voice_sustain && e->st.chans[MIXER_VOICE].n) {
        e->st.chans[MIXER_VOICE].n = 0;
        mixer_stop(e->mixer, 2, 0);
    }
    e->voice_fresh = false;
    e->st.voice_sustain = false;
}

static void do_play(vn_engine *e, const vn_audio_rec *a)
{
    chan_state *cs = &e->st.chans[a->channel];
    chan_state next = *cs;
    next.n = a->nassets > MIX_PLAYLIST_MAX ? MIX_PLAYLIST_MAX : a->nassets;
    for (int i = 0; i < next.n; i++) next.assets[i] = vn_le16(a->assets + 2 * i);
    next.loop = (a->flags & AUDIO_LOOP) != 0;
    bool queue = (a->flags & AUDIO_QUEUE) != 0;
    if ((a->flags & AUDIO_IF_CHANGED) && cs->n && next.n && cs->assets[0] == next.assets[0]) return;
    if (queue && cs->n) {
        for (int i = 0; i < next.n && cs->n < MIX_PLAYLIST_MAX; i++) cs->assets[cs->n++] = next.assets[i];
        cs->loop = next.loop;
    } else {
        *cs = next;
    }
    float fadeout = a->fadeout < 0 ? 0.0f : a->fadeout;
    start_channel(e, a->channel, &next, queue, a->fadein, fadeout, a->volume);
}

static void finish_host(vn_engine *e, const vm_ops *o)
{
    e->vm.pc += o->len;
}

static void host(vn_engine *e)
{
    vm *v = &e->vm;
    const vn_script *s = &e->script;
    vm_ops o;
    vn_buf t1 = {0}, t2 = {0};
    if (!vm_decode(v, &o)) {
        eng_fail(e, "bad instruction at %u", v->pc);
        return;
    }
    switch (o.op) {
    case OP_SAY: case OP_SAY_DYNAMIC: {
        uint16_t ci = (uint16_t)o.u[0];
        uint16_t tr = (uint16_t)(o.op == OP_SAY ? o.u[2] : o.u[1]);
        bool interact = ((o.op == OP_SAY ? o.u[3] : o.u[2]) & 1) != 0;
        if (ci >= s->nchars || (o.op == OP_SAY && o.u[1] >= s->nstrings)) {
            eng_fail(e, "bad say at %u", v->pc);
            break;
        }
        if (tr != VN_NONE16 && tr < s->ntrans && interact && !e->say_trans_done && !e->skip) {
            /* `e "..." with t`: run the transition first, then come back to this line. */
            e->say_trans_done = true;
            trans_start(e, &s->trans[tr]);
            e->mode = MODE_TRANSITION;
            break;
        }
        e->say_trans_done = false;
        if (interact) {
            rb_checkpoint(e);
            voice_line_start(e);
        }
        char *raw;
        if (o.op == OP_SAY) {
            raw = vn_strdup(s->strings[o.u[1]]);
        } else {
            value x = vm_pop(v);
            vn_buf b = {0};
            v_to_str(x, &b);
            vn_buf_u8(&b, 0);
            v_dec(x);
            raw = (char *)b.data;
        }
        const vn_char *c = &s->chars[ci];
        eng_trace(e, "say\t%s\t%s", esc(&t1, c->name == VN_NONE32 ? NULL : vn_str(s, c->name)), esc(&t2, raw));
        finish_host(e, &o);
        bool ok = start_say(e, ci, raw, interact);
        free(raw);
        if (!ok) break;
        if (e->mode == MODE_SAY) scene_copy(&e->committed, &e->st.sc);
        break;
    }
    case OP_SAY_ATTRS: {
        uint16_t ci = (uint16_t)o.u[0];
        if (ci >= s->nchars) {
            eng_fail(e, "bad say attributes at %u", v->pc);
            break;
        }
        if (s->chars[ci].flags & CHAR_EXTEND) ci = e->st.last_speaker;
        const vn_char *c = &s->chars[ci];
        const char *attrs[64];
        vn_buf names = {0};
        for (int i = 0; i < o.count && i < 64; i++) {
            attrs[i] = vn_str(s, vn_le32(o.list + 4 * i));
            if (i) vn_buf_u8(&names, ' ');
            vn_buf_put(&names, attrs[i], strlen(attrs[i]));
        }
        vn_buf_u8(&names, 0);
        eng_trace(e, "attrs\t%s\t%s", esc(&t1, c->name == VN_NONE32 ? NULL : vn_str(s, c->name)), (const char *)names.data);
        vn_buf_free(&names);
        if (c->image != VN_NONE32) {
            const char *tag = vn_str(s, c->image);
            sprite *sp = scene_find(&e->st.sc, tag);
            uint16_t before = sp ? sp->image : VN_NONE16;
            if (scene_set_attrs(&e->sctx, &e->st.sc, tag, attrs, o.count) && o.u[1] && !e->st.revert_pending) {
                e->st.revert_pending = true;
                snprintf(e->st.revert_key, sizeof e->st.revert_key, "%s", tag);
                e->st.revert_image = before;
            }
        }
        finish_host(e, &o);
        break;
    }
    case OP_MENU: {
        if (v->sp < o.count) {
            eng_fail(e, "stack underflow at menu %u", v->pc);
            break;
        }
        int visible = 0;
        for (int i = 0; i < o.count; i++) visible += v_truthy(vm_peek(v, (uint32_t)(o.count - 1 - i)));
        if (visible) rb_checkpoint(e);
        for (int i = 0; i < e->nitems; i++) free(e->items[i].text);
        e->nitems = 0;
        bool conds[256];
        for (int i = o.count - 1; i >= 0; i--) {
            value x = vm_pop(v);
            conds[i] = v_truthy(x);
            v_dec(x);
        }
        bool failed = false;
        for (int i = 0; i < o.count && !failed; i++) {
            if (!conds[i]) continue;
            uint32_t sid = vn_le32(o.list + 8 * i), addr = vn_le32(o.list + 8 * i + 4);
            if (addr >= s->code_size || e->nitems >= MENU_MAX) {
                eng_fail(e, "bad menu at %u", v->pc);
                failed = true;
                break;
            }
            char *txt = interpolate(e, vn_str(s, sid));
            if (!txt) {
                failed = true;
                break;
            }
            e->items[e->nitems].addr = addr;
            e->items[e->nitems].text = txt;
            e->nitems++;
        }
        if (failed) break;
        finish_host(e, &o);
        if (!visible) {
            e->st.caption = false;
            break;
        }
        e->cursor = 0;
        voice_line_start(e);
        scene_copy(&e->committed, &e->st.sc);
        e->mode = MODE_MENU;
        break;
    }
    case OP_PAUSE: {
        if (v->sp < 1) {
            eng_fail(e, "stack underflow at pause %u", v->pc);
            break;
        }
        value d = vm_peek(v, 0);
        bool click = d.t == V_NONE;
        if (click) rb_checkpoint(e);
        d = vm_pop(v);
        vn_buf b = {0};
        v_repr(d, &b);
        vn_buf_u8(&b, 0);
        eng_trace(e, "pause\t%s", (const char *)b.data);
        vn_buf_free(&b);
        if (!click && d.t != V_INT && d.t != V_FLOAT && d.t != V_BOOL) {
            v_dec(d);
            eng_fail(e, "TypeError: pause delay must be a number");
            break;
        }
        e->pause_left = click ? -1.0f : (float)(d.t == V_FLOAT ? d.u.f : d.t == V_INT ? (double)d.u.i : d.u.b);
        v_dec(d);
        finish_host(e, &o);
        scene_copy(&e->committed, &e->st.sc);
        e->mode = MODE_PAUSE;
        break;
    }
    case OP_WITH: {
        uint16_t tr = (uint16_t)o.u[0];
        if (tr == VN_NONE16)
            eng_trace(e, "with\tNone");
        else
            eng_trace(e, "with\t%u", tr);
        finish_host(e, &o);
        if (tr == VN_NONE16 || tr >= s->ntrans || e->skip || trans_duration(&s->trans[tr]) <= 0) {
            scene_copy(&e->committed, &e->st.sc);
            break;
        }
        trans_start(e, &s->trans[tr]);
        e->mode = MODE_TRANSITION;
        break;
    }
    case OP_SHOW: case OP_SCENE: case OP_HIDE: case OP_SHOW_DYNAMIC: {
        uint16_t ri = (uint16_t)o.u[0];
        if (ri != VN_NONE16 && ri >= s->nshows) {
            eng_fail(e, "bad show record at %u", v->pc);
            break;
        }
        const vn_show *rec = ri == VN_NONE16 ? NULL : &s->shows[ri];
        if (o.op == OP_SHOW_DYNAMIC) {
            value name = vm_pop(v);
            if (name.t != V_STR || !rec) {
                v_dec(name);
                eng_fail(e, "show expression needs an image name string");
                break;
            }
            eng_trace(e, "show\t%s\t", esc(&t1, v_as_str(name)->data));
            char buf[256];
            snprintf(buf, sizeof buf, "%s", v_as_str(name)->data);
            v_dec(name);
            const char *words[32];
            int nw = 0;
            for (char *p = strtok(buf, " "); p && nw < 32; p = strtok(NULL, " ")) words[nw++] = p;
            if (nw) scene_show(&e->sctx, &e->st.sc, rec, words[0], words + 1, nw - 1);
        } else if (o.op == OP_HIDE) {
            trace_show(e, "hide", rec);
            if (rec) scene_hide(&e->st.sc, vn_str(s, rec->tag));
        } else {
            trace_show(e, o.op == OP_SCENE ? "scene" : "show", rec);
            if (o.op == OP_SCENE) scene_clear(&e->st.sc);
            if (rec) scene_show(&e->sctx, &e->st.sc, rec, NULL, NULL, 0);
        }
        finish_host(e, &o);
        break;
    }
    case OP_PLAY: {
        uint16_t ai = (uint16_t)o.u[0];
        if (ai >= s->naudio) {
            eng_fail(e, "bad play record at %u", v->pc);
            break;
        }
        eng_trace(e, "play\t%u", ai);
        do_play(e, &s->audio[ai]);
        finish_host(e, &o);
        break;
    }
    case OP_STOP: {
        uint8_t ch = (uint8_t)o.u[0];
        eng_trace(e, "stop\t%u", ch);
        if (ch < s->nchannels && ch < MIX_MAX_CHANNELS) {
            e->st.chans[ch].n = 0;
            mixer_stop(e->mixer, ch, o.f < 0 ? 0.0f : o.f);
        }
        finish_host(e, &o);
        break;
    }
    case OP_VOICE: {
        uint16_t ai = (uint16_t)o.u[0];
        eng_trace(e, "voice\t%u", ai);
        if (ai < s->nassets) {
            chan_state cs = {1, {ai}, false, e->st.chans[MIXER_VOICE].volume};
            e->st.chans[MIXER_VOICE] = cs;
            start_channel(e, 2, &cs, false, 0, 0, 1.0f);
            e->voice_fresh = true;
        }
        finish_host(e, &o);
        break;
    }
    case OP_VOICE_SUSTAIN:
        eng_trace(e, "voice_sustain");
        e->st.voice_sustain = true;
        e->voice_fresh = true;
        finish_host(e, &o);
        break;
    case OP_WINDOW: case OP_NVL: {
        eng_trace(e, "%s\t%u\t%u", o.op == OP_WINDOW ? "window" : "nvl", o.u[0], o.u[1]);
        if (o.op == OP_WINDOW) {
            e->st.window = o.u[0] == 0 ? WINDOW_SHOW : o.u[0] == 1 ? WINDOW_HIDE : WINDOW_AUTO;
        } else if (o.u[0] == 0) {
            for (uint32_t i = 0; i < e->st.nnvl; i++) {
                free(e->st.nvl[i].who);
                free(e->st.nvl[i].text);
            }
            e->st.nnvl = 0;
        }
        finish_host(e, &o);
        break;
    }
    case OP_MOVIE:
        eng_trace(e, "movie\t%u", o.u[0]);
        if (!e->warned_movie) vn_log("vnpsp: movie cutscenes are not supported yet; skipping");
        e->warned_movie = true;
        finish_host(e, &o);
        break;
    case OP_CALL_FN: {
        uint8_t fn = (uint8_t)o.u[0], argc = (uint8_t)o.u[1];
        static const char *const names[FN_COUNT] = {
            [FN_INPUT] = "renpy.input", [FN_NOTIFY] = "renpy.notify", [FN_FULL_RESTART] = "renpy.full_restart",
            [FN_BLOCK_ROLLBACK] = "renpy.block_rollback", [FN_SAVE_PERSISTENT] = "renpy.save_persistent",
            [FN_FORCE_AUTOSAVE] = "renpy.force_autosave", [FN_SHOWING] = "renpy.showing",
            [FN_SET_VOLUME] = "renpy.music.set_volume"};
        if (fn == FN_INPUT) {
            rb_checkpoint(e);
            value prompt = vm_peek(v, 2), dflt = vm_peek(v, 1), len = vm_peek(v, 0);
            vn_buf b = {0};
            v_to_str(prompt, &b);
            vn_buf_u8(&b, 0);
            char *p = interpolate(e, (const char *)b.data);
            vn_buf_free(&b);
            if (!p) break;
            snprintf(e->input_prompt, sizeof e->input_prompt, "%s", p);
            free(p);
            e->input_len = 0;
            if (dflt.t == V_STR) {
                size_t n = v_as_str(dflt)->len;
                if (n > INPUT_MAX) n = INPUT_MAX;
                memcpy(e->input_buf, v_as_str(dflt)->data, n);
                e->input_len = (int)n;
            }
            e->input_buf[e->input_len] = 0;
            e->input_max = len.t == V_INT && len.u.i > 0 && len.u.i < INPUT_MAX ? (int)len.u.i : INPUT_MAX;
            e->osk_row = e->osk_col = 0;
            voice_line_start(e);
            scene_copy(&e->committed, &e->st.sc);
            e->mode = MODE_INPUT;
            break;
        }
        vn_buf args = {0};
        value list = v_list_new(V_LIST, argc);
        for (int i = argc - 1; i >= 0; i--) v_list_push(list, v_inc(vm_peek(v, (uint32_t)i)));
        v_repr(list, &args);
        vn_buf_u8(&args, 0);
        eng_trace(e, "call\t%s\t%s", names[fn] ? names[fn] : "?", esc(&t1, (const char *)args.data));
        vn_buf_free(&args);
        value result = v_none();
        vlist *a = v_as_list(list);
        switch (fn) {
        case FN_NOTIFY: {
            vn_buf b = {0};
            v_to_str(a->items[0], &b);
            vn_buf_u8(&b, 0);
            snprintf(e->notify, sizeof e->notify, "%s", (const char *)b.data);
            e->notify_t = 3.0f;
            vn_buf_free(&b);
            break;
        }
        case FN_BLOCK_ROLLBACK: rb_clear(e); break;
        case FN_SAVE_PERSISTENT: persistent_save(e, true); break;
        case FN_FORCE_AUTOSAVE:
            if (!save_write(e, "auto.sav")) vn_log("vnpsp: autosave failed");
            break;
        case FN_SHOWING:
            result = v_bool(a->items[0].t == V_STR && scene_showing(s, &e->st.sc, v_as_str(a->items[0])->data));
            break;
        case FN_SET_VOLUME: {
            value vol = a->items[0], delay = a->items[1], ch = a->items[2];
            float fv = vol.t == V_FLOAT ? (float)vol.u.f : vol.t == V_INT ? (float)vol.u.i : 1.0f;
            float fd = delay.t == V_FLOAT ? (float)delay.u.f : delay.t == V_INT ? (float)delay.u.i : 0.0f;
            if (ch.t == V_INT && ch.u.i >= 0 && (uint64_t)ch.u.i < s->nchannels && ch.u.i < MIX_MAX_CHANNELS) {
                e->st.chans[ch.u.i].volume = vn_clampf(fv, 0, 1);
                mixer_channel_volume(e->mixer, (int)ch.u.i, vn_clampf(fv, 0, 1), fd);
            }
            break;
        }
        default: break;
        }
        v_dec(list);
        for (int i = 0; i < argc; i++) v_dec(vm_pop(v));
        if (fn == FN_FULL_RESTART) {
            eng_to_main_menu(e);
            break;
        }
        vm_push(v, result);
        finish_host(e, &o);
        break;
    }
    default:
        eng_fail(e, "unexpected host instruction %u at %u", o.op, v->pc);
        break;
    }
    vn_buf_free(&t1);
    vn_buf_free(&t2);
}

static bool interaction_mode(vn_mode m);

static void run_story(vn_engine *e)
{
    for (int guard = 0; guard < 100000 && e->mode == MODE_RUN; guard++) {
        vm_status st = vm_run(&e->vm);
        if (e->vm.last_jump != VN_NONE32) {
            prefetch(e, e->vm.last_jump);
            e->vm.last_jump = VN_NONE32;
        }
        switch (st) {
        case VM_HOST:
            host(e);
            if (interaction_mode(e->mode)) e->interactions++;
            break;
        case VM_END:
            eng_trace(e, "end");
            eng_to_main_menu(e);
            return;
        case VM_ERROR: {
            const vn_label *best = NULL;
            for (uint32_t i = 0; i < e->script.nlabels; i++) {
                const vn_label *l = &e->script.labels[i];
                if (l->addr <= e->vm.pc && (!best || l->addr > best->addr)) best = l;
            }
            eng_fail(e, "%s (at %u, in label %s)", e->vm.err.msg, e->vm.pc,
                     best ? vn_str(&e->script, best->name) : "?");
            return;
        }
        default:
            eng_fail(e, "the story stopped unexpectedly");
            return;
        }
    }
}

/* ---- update ----------------------------------------------------------------------- */

static bool interaction_mode(vn_mode m)
{
    return m == MODE_SAY || m == MODE_MENU || m == MODE_PAUSE || m == MODE_INPUT;
}

void vn_engine_update(vn_engine *e, const vn_input *in, float dt)
{
    e->frame++;
    vn_assets_new_frame(&e->assets);
    e->blink += dt;
    if (e->message_t > 0) e->message_t -= dt;
    if (e->notify_t > 0) e->notify_t -= dt;

    switch (e->mode) {
    case MODE_MAIN_MENU: case MODE_GAME_MENU: case MODE_SAVE: case MODE_LOAD: case MODE_SETTINGS:
    case MODE_HISTORY: case MODE_CONFIRM:
        ui_update(e, in);
        if (e->mode == MODE_RUN) run_story(e);
        return;
    case MODE_ERROR:
        if (in->pressed & (VN_CROSS | VN_CIRCLE | VN_START)) eng_to_main_menu(e);
        return;
    case MODE_QUIT:
        return;
    default:
        break;
    }

    e->skip = (in->held & VN_R) != 0;
    if (interaction_mode(e->mode) && e->mode != MODE_INPUT) {
        if (in->pressed & VN_START) {
            thumb_capture(e);
            ui_open(e, MODE_GAME_MENU);
            return;
        }
        if (in->pressed & VN_SELECT) {
            thumb_capture(e);
            eng_message(e, save_write(e, "quick.sav") ? "Quick saved." : "Quick save failed.");
        }
        if (in->pressed & VN_TRIANGLE) {
            ui_open(e, MODE_HISTORY);
            return;
        }
        if (in->pressed & VN_SQUARE) {
            e->auto_mode = !e->auto_mode;
            eng_message(e, e->auto_mode ? "Auto mode on." : "Auto mode off.");
        }
        if (e->mode == MODE_SAY && (in->pressed & VN_CIRCLE)) e->box_hidden = !e->box_hidden;
    }
    /* Rollback works from every interaction, the name keyboard included (it does not use L). */
    if (interaction_mode(e->mode) && (in->pressed & VN_L)) {
        if (rb_back(e)) {
            eng_trace(e, "rollback");
            e->mode = MODE_RUN;
            run_story(e);
        }
        return;
    }

    e->st.clock += dt;
    e->sctx.clock = e->st.clock;
    switch (e->mode) {
    case MODE_RUN: break;
    case MODE_SAY: say_update(e, in, dt); break;
    case MODE_MENU: choice_update(e, in); break;
    case MODE_INPUT: osk_update(e, in); break;
    case MODE_PAUSE:
        if (e->pause_left >= 0) {
            e->pause_left -= dt;
            if (e->pause_left <= 0) e->mode = MODE_RUN;
        }
        if ((in->pressed & VN_CROSS) || e->skip) e->mode = MODE_RUN;
        break;
    case MODE_TRANSITION:
        if (trans_update(e, dt) || (in->pressed & VN_CROSS) || e->skip) {
            trans_finish(e);
            e->mode = MODE_RUN;
        }
        break;
    default: break;
    }
    if (e->mode == MODE_RUN) run_story(e);
}

/* ---- drawing ------------------------------------------------------------------------ */

void draw_panel(float x, float y, float w, float h, uint32_t fill, uint32_t border)
{
    gfx_fill_rect(x, y, w, h, fill);
    if (border >> 24) {
        gfx_fill_rect(x, y, w, 1, border);
        gfx_fill_rect(x, y + h - 1, w, 1, border);
        gfx_fill_rect(x, y + 1, 1, h - 2, border);
        gfx_fill_rect(x + w - 1, y + 1, 1, h - 2, border);
    }
}

static void draw_text_disp(void *user, uint16_t disp, const layout *l)
{
    vn_engine *e = user;
    const vn_disp *d = &e->script.disps[disp];
    text_layout tl;
    text_layout_init(&tl);
    const char *s = vn_str(&e->script, d->text);
    text_style ts = {&e->assets, eng_font(e, FONT_TEXT), gfx_rgba(255, 255, 255, 255), gfx_rgba(0, 0, 0, 255), 440, true,
                     base_text_size(e)};
    text_layout_build(&tl, s, strlen(s), &ts);
    text_draw(&tl, floorf(l->tx + 0.5f), floorf(l->ty + 0.5f), tl.n, l->alpha, gfx_rgba(0, 0, 0, 255));
    text_layout_free(&tl);
}

static void measure_text_disp(void *user, uint16_t disp, float *w, float *h)
{
    vn_engine *e = user;
    const vn_disp *d = &e->script.disps[disp];
    text_layout tl;
    text_layout_init(&tl);
    const char *s = vn_str(&e->script, d->text);
    text_style ts = {&e->assets, eng_font(e, FONT_TEXT), 0xFFFFFFFFu, 0, 440, true, base_text_size(e)};
    text_layout_build(&tl, s, strlen(s), &ts);
    *w = tl.width;
    *h = tl.height;
    text_layout_free(&tl);
}

static void draw_ctc(vn_engine *e, float x, float y)
{
    /* Story time, not wall time, so a restored state draws the same frame. */
    float a = 0.5f + 0.5f * sinf((float)e->st.clock * 6.0f);
    gfx_fill_rect(x, y, 6, 6, gfx_rgba(255, 255, 255, (uint32_t)(80 + 175 * a)));
}

static void draw_box_text(vn_engine *e)
{
    const vn_char *c = speaker(e);
    const vn_font *nf = eng_font(e, FONT_NAME);
    const vn_font *tf = eng_font(e, FONT_TEXT);
    float lines = (float)(e->say.nlines > BOX_MIN_LINES ? e->say.nlines : BOX_MIN_LINES);
    float text_h = e->say.nlines > BOX_MIN_LINES ? e->say.height : lines * tf->line_h;
    float name_h = e->say_who ? (float)nf->line_h + 2 : 0;
    float h = text_h + name_h + 2 * BOX_PAD;
    float y = GFX_H - BOX_MARGIN - h;
    if (y < BOX_MARGIN) y = BOX_MARGIN;
    draw_panel(BOX_MARGIN, y, GFX_W - 2 * BOX_MARGIN, h, gfx_rgba(0, 0, 0, 180), gfx_rgba(255, 255, 255, 60));
    float side = side_width(e, c);
    if (side > 0) scene_draw_disp_at(&e->sctx, side_disp(e, c), 0, GFX_H, 1.0f);
    float x = BOX_MARGIN + BOX_PAD + (side > 0 ? side + 4 : 0);
    if (e->say_who) text_draw_plain(nf, e->say_who, x, y + BOX_PAD, e->say_who_color);
    text_draw(&e->say, x, y + BOX_PAD + name_h, (uint32_t)e->reveal, 1.0f, gfx_rgba(0, 0, 0, 255));
    if (e->say_done && e->say.n) {
        const tglyph *g = &e->say.g[e->say.n - 1];
        draw_ctc(e, x + g->x + g->adv + 3, y + BOX_PAD + name_h + g->y - 6);
    }
}

static void draw_nvl(vn_engine *e, bool reveal_last)
{
    gfx_fill_rect(0, 0, GFX_W, GFX_H, gfx_rgba(0, 0, 0, 190));
    const vn_font *tf = eng_font(e, FONT_TEXT);
    text_style ts = {&e->assets, tf, gfx_rgba(255, 255, 255, 255), gfx_rgba(0, 0, 0, 255), GFX_W - 32, false,
                     base_text_size(e)};
    /* Lay out every entry, then draw the newest ones that fit, oldest first. */
    float heights[256];
    uint32_t n = e->st.nnvl < 256 ? e->st.nnvl : 256, first = e->st.nnvl - n;
    float total = 0;
    uint32_t start = e->st.nnvl;
    for (uint32_t i = e->st.nnvl; i-- > first;) {
        text_layout tl;
        text_layout_init(&tl);
        vn_buf b = {0};
        if (e->st.nvl[i].who) {
            vn_buf_put(&b, e->st.nvl[i].who, strlen(e->st.nvl[i].who));
            vn_buf_put(&b, ": ", 2);
        }
        vn_buf_put(&b, e->st.nvl[i].text, strlen(e->st.nvl[i].text));
        text_layout_build(&tl, (const char *)b.data, b.len, &ts);
        heights[i - first] = tl.height + 4;
        vn_buf_free(&b);
        text_layout_free(&tl);
        if (total + heights[i - first] > GFX_H - 24 && start != e->st.nnvl) break;
        total += heights[i - first];
        start = i;
    }
    float y = 12;
    for (uint32_t i = start; i < e->st.nnvl; i++) {
        bool last = i + 1 == e->st.nnvl;
        if (last && reveal_last && e->mode == MODE_SAY) {
            /* The newest entry is the one being typed: reuse the say layout and its reveal. */
            if (e->say_who) {
                text_draw_plain(tf, e->say_who, 16, y, e->st.nvl[i].color);
                y += tf->line_h;
            }
            text_draw(&e->say, 16, y, (uint32_t)e->reveal, 1.0f, gfx_rgba(0, 0, 0, 255));
            if (e->say_done && e->say.n) {
                const tglyph *g = &e->say.g[e->say.n - 1];
                draw_ctc(e, 16 + g->x + g->adv + 3, y + g->y - 6);
            }
            break;
        }
        text_layout tl;
        text_layout_init(&tl);
        if (e->st.nvl[i].who) {
            text_draw_plain(tf, e->st.nvl[i].who, 16, y, e->st.nvl[i].color);
            y += tf->line_h;
        }
        text_layout_build(&tl, e->st.nvl[i].text, strlen(e->st.nvl[i].text), &ts);
        text_draw(&tl, 16, y, tl.n, 1.0f, gfx_rgba(0, 0, 0, 255));
        y += tl.height + 4;
        text_layout_free(&tl);
    }
}

static void draw_toast(const vn_font *f, const char *msg, float y, float alpha)
{
    float w = text_measure_plain(f, msg) + 16;
    if (w > GFX_W - 16) w = GFX_W - 16;
    float x = (GFX_W - w) / 2;
    uint32_t a = (uint32_t)(vn_clampf(alpha, 0, 1) * 255);
    draw_panel(x, y, w, (float)f->line_h + 8, gfx_rgba(0, 0, 0, a * 200 / 255), gfx_rgba(255, 255, 255, a / 3));
    text_draw_plain(f, msg, x + 8, y + 4, gfx_rgba(255, 255, 255, a));
}

void eng_draw_story(vn_engine *e)
{
    if (e->snapshot_pending) {
        gfx_target_begin(0, gfx_rgba(0, 0, 0, 255));
        scene_draw(&e->sctx, &e->committed, 0, 0, NULL);
        gfx_target_end();
        e->snapshot_pending = false;
    }
    if (e->trans && e->mode == MODE_TRANSITION)
        trans_draw(e);
    else
        scene_draw(&e->sctx, &e->st.sc, 0, 0, NULL);

    vn_mode m = e->mode;
    if (m >= MODE_GAME_MENU && m <= MODE_CONFIRM) m = e->overlay_return;
    const vn_char *c = speaker(e);
    bool nvl_say = m == MODE_SAY && c->kind == 1;
    if (e->st.nnvl && (nvl_say || m == MODE_MENU)) draw_nvl(e, nvl_say);
    if (m == MODE_SAY && !e->box_hidden && !nvl_say) {
        if (c->flags & CHAR_CENTERED) {
            text_draw(&e->say, (GFX_W - say_width(e)) / 2, (GFX_H - e->say.height) / 2, (uint32_t)e->reveal, 1.0f,
                      gfx_rgba(0, 0, 0, 255));
        } else {
            draw_box_text(e);
        }
    } else if (m == MODE_MENU && e->st.caption && e->st.caption_text) {
        uint16_t keep = e->say_char;
        e->say_char = e->st.caption_char;
        const vn_char *cc = speaker(e);
        char *who = NULL;
        if (cc->name != VN_NONE32) who = vn_strdup(vn_str(&e->script, cc->name));
        text_layout saved = e->say;
        char *saved_who = e->say_who;
        float saved_reveal = e->reveal;
        bool saved_done = e->say_done;
        text_layout_init(&e->say);
        layout_say(e, e->st.caption_text, char_color(cc->what_color, gfx_rgba(255, 255, 255, 255)));
        e->say_who = who;
        e->reveal = (float)e->say.n;
        e->say_done = false;
        draw_box_text(e);
        text_layout_free(&e->say);
        free(who);
        e->say = saved;
        e->say_who = saved_who;
        e->reveal = saved_reveal;
        e->say_done = saved_done;
        e->say_char = keep;
    } else if (e->st.window == WINDOW_SHOW && m != MODE_SAY && m != MODE_MENU) {
        draw_panel(BOX_MARGIN, GFX_H - BOX_MARGIN - 70, GFX_W - 2 * BOX_MARGIN, 70, gfx_rgba(0, 0, 0, 180),
                   gfx_rgba(255, 255, 255, 60));
    }
    if (m == MODE_MENU) choice_draw(e);
    if (m == MODE_INPUT) osk_draw(e);
    const vn_font *uf = eng_font(e, FONT_INTERFACE);
    if (e->notify_t > 0) draw_toast(uf, e->notify, 8, e->notify_t);
    if (e->auto_mode) text_draw_plain(uf, "Auto", GFX_W - 40, 4, gfx_rgba(255, 255, 255, 200));
    if (e->skip) text_draw_plain(uf, "Skip", GFX_W - 80, 4, gfx_rgba(255, 255, 255, 200));
}

void vn_engine_render(vn_engine *e)
{
    gfx_frame_begin(gfx_rgba(0, 0, 0, 255));
    vn_mode m = e->mode;
    if (m == MODE_MAIN_MENU || (m >= MODE_SAVE && m <= MODE_CONFIRM && e->overlay_return == MODE_MAIN_MENU)) {
        ui_draw(e);
    } else if (m == MODE_ERROR) {
        const vn_font *f = eng_font(e, FONT_INTERFACE);
        text_layout tl;
        text_layout_init(&tl);
        text_style ts = {&e->assets, f, gfx_rgba(255, 200, 200, 255), 0, GFX_W - 40, false, 22};
        vn_buf b = {0};
        const char *head = "An error stopped the story:\n\n";
        vn_buf_put(&b, head, strlen(head));
        for (const char *p = e->err; *p; p++) {
            vn_buf_u8(&b, (uint8_t)*p);
            if (*p == '{') vn_buf_u8(&b, '{');
        }
        vn_buf_put(&b, "\n\nPress X to return to the main menu.", 37);
        text_layout_build(&tl, (const char *)b.data, b.len, &ts);
        text_draw(&tl, 20, 20, tl.n, 1.0f, 0);
        text_layout_free(&tl);
        vn_buf_free(&b);
    } else {
        eng_draw_story(e);
        if (m >= MODE_GAME_MENU && m <= MODE_CONFIRM) ui_draw(e);
    }
    if (e->message_t > 0) draw_toast(eng_font(e, FONT_INTERFACE), e->message, GFX_H - 40, e->message_t);
    gfx_frame_end();
}

/* ---- lifetime ----------------------------------------------------------------------- */

vn_engine *vn_engine_new(const vn_config *cfg, char *err, size_t errcap)
{
    vn_engine *e = vn_calloc(1, sizeof *e);
    e->cfg = *cfg;
    e->data_dir = vn_strdup(cfg->data_dir);
    e->save_dir = vn_strdup(cfg->save_dir);
    e->cfg.data_dir = e->data_dir;
    e->cfg.save_dir = e->save_dir;
    char path[512];
    vn_path_join(path, sizeof path, cfg->data_dir, "script.vnb");
    void *data;
    size_t size;
    if (!vn_fs_read_all(path, &data, &size)) {
        snprintf(err, errcap, "cannot read %s", path);
        free(e->data_dir);
        free(e->save_dir);
        free(e);
        return NULL;
    }
    if (!vn_script_load(&e->script, data, size, err, errcap)) {
        vn_script_free(&e->script);
        free(e->data_dir);
        free(e->save_dir);
        free(e);
        return NULL;
    }
    vn_path_join(path, sizeof path, cfg->data_dir, "data.pak");
    if (!vn_assets_open(&e->assets, path, &e->script, cfg->texture_budget ? cfg->texture_budget : (12u << 20), err,
                        errcap)) {
        vn_assets_close(&e->assets);
        vn_script_free(&e->script);
        free(e->data_dir);
        free(e->save_dir);
        free(e);
        return NULL;
    }
    vm_init(&e->vm, &e->script);
    scene_init(&e->st.sc);
    scene_init(&e->committed);
    e->sctx.s = &e->script;
    e->sctx.assets = &e->assets;
    e->sctx.v = scene_view(&e->script);
    e->sctx.draw_text = draw_text_disp;
    e->sctx.measure_text = measure_text_disp;
    e->sctx.user = e;
    uint8_t mixers[MIX_MAX_CHANNELS];
    for (uint32_t i = 0; i < e->script.nchannels && i < MIX_MAX_CHANNELS; i++) mixers[i] = e->script.channels[i].mixer;
    e->mixer = mixer_new((int)e->script.nchannels, mixers);
    for (int i = 0; i < MIX_MAX_CHANNELS; i++) e->st.chans[i].volume = 1.0f;
    e->st.window = WINDOW_AUTO;
    e->st.revert_image = VN_NONE16;
    e->wait_until = UINT32_MAX;
    vn_fs_mkdir(e->save_dir);
    settings_load(e);
    settings_apply(e);
    if (!vm_run_routine(&e->vm, e->script.init_addr)) {
        snprintf(err, errcap, "init: %s", e->vm.err.msg);
        vn_engine_free(e);
        return NULL;
    }
    persistent_load(e);
    e->mode = MODE_MAIN_MENU;
    return e;
}

void vn_engine_free(vn_engine *e)
{
    if (!e) return;
    if (e->mode != MODE_ERROR && e->script.nsyms) persistent_save(e, false);
    story_reset(e);
    scene_free(&e->st.sc);
    scene_free(&e->committed);
    rb_free(e);
    history_free(e);
    for (int i = 0; i < SAVE_SLOTS; i++) {
        gfx_tex_destroy(e->slot_tex[i]);
        free(e->slot_rgba[i]);
    }
    mixer_free(e->mixer);
    vm_free(&e->vm);
    vn_assets_close(&e->assets);
    vn_script_free(&e->script);
    free(e->data_dir);
    free(e->save_dir);
    free(e);
}

/* ---- introspection ----------------------------------------------------------------- */

vn_mixer *vn_engine_mixer(vn_engine *e)
{
    return e->mixer;
}

const char *vn_engine_title(const vn_engine *e)
{
    return e->script.title != VN_NONE32 ? vn_str(&e->script, e->script.title) : "vnpsp";
}

vn_mode vn_engine_mode(const vn_engine *e)
{
    return e->mode;
}

int vn_engine_cursor(const vn_engine *e, int *count)
{
    if (e->mode == MODE_MENU) {
        if (count) *count = e->nitems;
        return e->cursor;
    }
    if (count) *count = 0;
    return e->ui_cursor;
}

bool vn_engine_text_complete(const vn_engine *e)
{
    return e->say_done;
}

const char *vn_engine_error(const vn_engine *e)
{
    return e->err;
}

uint64_t vn_engine_state_hash(vn_engine *e)
{
    return e->current.data ? vn_fnv1a64(e->current.data, e->current.len) : 0;
}

void vn_engine_asset_stats(const vn_engine *e, uint32_t *loads, uint64_t *bytes, size_t *cached)
{
    *loads = e->assets.loads;
    *bytes = e->assets.bytes_read;
    *cached = e->assets.used;
}

uint32_t vn_engine_interactions(const vn_engine *e)
{
    return e->interactions;
}

int vn_engine_rollback_depth(const vn_engine *e)
{
    return e->ring_count;
}

void vn_engine_osk_cursor(const vn_engine *e, int *row, int *col)
{
    *row = e->osk_row;
    *col = e->osk_col;
}

const char *vn_engine_osk_text(const vn_engine *e)
{
    return e->input_buf;
}
