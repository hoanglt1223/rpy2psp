/* Checkpoints (the whole story state as bytes), save files, and persistent data.
 *
 * Save file: "VNSV", u16 version, u16 0, u64 script hash, u64 unix time,
 * u16 thumb w, u16 thumb h, RGB565 thumbnail, str description, u32 history
 * count + (str who, str text) each, u32 checkpoint length + checkpoint,
 * u32 CRC-32 of everything before it. A save only loads into the exact
 * script.vnb it was made with (code addresses are stored). */
#include "engine_internal.h"
#include "fs.h"
#include "gfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CK_VERSION 1
#define SAVE_VERSION 1

static void put_str(vn_buf *b, const char *s)
{
    if (!s) {
        vn_buf_u32(b, VN_NONE32);
        return;
    }
    vn_buf_str(b, s, strlen(s));
}

static char *get_str(vn_rd *r)
{
    uint32_t n = vn_rd_u32(r);
    if (n == VN_NONE32) return NULL;
    const uint8_t *p = vn_rd_bytes(r, n);
    if (!p) return NULL;
    char *s = vn_alloc(n + 1);
    memcpy(s, p, n);
    s[n] = 0;
    return s;
}

static void put_f32(vn_buf *b, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    vn_buf_u32(b, u);
}

void ck_serialize(vn_engine *e, vn_buf *out)
{
    const vn_script *s = &e->script;
    vm *v = &e->vm;
    vn_buf_put(out, "VNCK", 4);
    vn_buf_u32(out, CK_VERSION);
    vn_buf_u32(out, v->pc);
    vser *ser = vser_new(out);
    vn_buf_u32(out, v->sp);
    for (uint32_t i = 0; i < v->sp; i++) vser_value(ser, v->stack[i]);
    vn_buf_u32(out, v->nframes);
    for (uint32_t i = 0; i < v->nframes; i++) {
        vn_buf_u32(out, v->frames[i].ret);
        vn_buf_u32(out, v->frames[i].nsaved);
        for (uint32_t k = 0; k < v->frames[i].nsaved; k++) {
            vn_buf_u16(out, v->frames[i].saved[k].sym);
            vser_value(ser, v->frames[i].saved[k].old);
        }
    }
    vn_buf_u32(out, s->nsyms);
    for (uint32_t i = 0; i < s->nsyms; i++) vser_value(ser, s->sym_kind[i] == 1 ? v_unset() : v->vars[i]);
    vser_free(ser);
    vn_buf_u8(out, v->uses_random);
    if (v->uses_random) {
        for (int i = 0; i < 624; i++) vn_buf_u32(out, v->rng.mt[i]);
        vn_buf_u32(out, v->rng.index);
    }

    story *st = &e->st;
    vn_buf_f64(out, st->clock);
    vn_buf_u8(out, st->window);
    vn_buf_u16(out, st->last_speaker);
    put_str(out, st->last_text);
    vn_buf_u8(out, st->caption);
    vn_buf_u16(out, st->caption_char);
    put_str(out, st->caption_text);
    vn_buf_u8(out, st->revert_pending);
    put_str(out, st->revert_key);
    vn_buf_u16(out, st->revert_image);
    vn_buf_u32(out, st->nnvl);
    for (uint32_t i = 0; i < st->nnvl; i++) {
        put_str(out, st->nvl[i].who);
        put_str(out, st->nvl[i].text);
        vn_buf_u32(out, st->nvl[i].color);
    }
    vn_buf_u8(out, st->voice_sustain);
    vn_buf_u32(out, s->nchannels);
    for (uint32_t i = 0; i < s->nchannels && i < MIX_MAX_CHANNELS; i++) {
        const chan_state *c = &st->chans[i];
        vn_buf_u8(out, c->n);
        for (int k = 0; k < c->n; k++) vn_buf_u16(out, c->assets[k]);
        vn_buf_u8(out, c->loop);
        put_f32(out, c->volume);
    }
    vn_buf_u32(out, st->sc.n);
    for (uint32_t i = 0; i < st->sc.n; i++) {
        const sprite *sp = &st->sc.items[i];
        put_str(out, sp->key);
        vn_buf_u16(out, sp->image);
        vn_buf_u16(out, sp->disp);
        vn_buf_u8(out, sp->nat);
        for (int k = 0; k < sp->nat; k++) vn_buf_u16(out, sp->at[k]);
        vn_buf_u16(out, sp->atl);
        vn_buf_u32(out, (uint32_t)sp->zorder);
        vn_buf_f64(out, st->clock - sp->shown_at);
    }
}

static void free_story_strings(story *st)
{
    free(st->last_text);
    free(st->caption_text);
    for (uint32_t i = 0; i < st->nnvl; i++) {
        free(st->nvl[i].who);
        free(st->nvl[i].text);
    }
    free(st->nvl);
    st->last_text = st->caption_text = NULL;
    st->nvl = NULL;
    st->nnvl = 0;
}

bool ck_restore(vn_engine *e, const uint8_t *data, size_t len)
{
    const vn_script *s = &e->script;
    vn_rd r;
    vn_rd_init(&r, data, len);
    const uint8_t *magic = vn_rd_bytes(&r, 4);
    if (!magic || memcmp(magic, "VNCK", 4) != 0 || vn_rd_u32(&r) != CK_VERSION) return false;
    uint32_t pc = vn_rd_u32(&r);
    if (pc >= s->code_size) return false;

    /* Everything is parsed into temporaries first so a bad blob changes nothing. */
    bool ok = true;
    vdeser *d = vdeser_new(&r);
    uint32_t sp = vn_rd_u32(&r);
    value *stack = NULL, *vars = NULL;
    vm_frame *frames = NULL;
    uint32_t nframes = 0, nsyms = 0, nnvl = 0;
    story st;
    memset(&st, 0, sizeof st);
    if (r.bad || sp > 4096) ok = false;
    if (ok) {
        stack = vn_calloc(sp + 1, sizeof(value));
        for (uint32_t i = 0; ok && i < sp; i++) ok = vdeser_value(d, &stack[i]);
    }
    if (ok) {
        nframes = vn_rd_u32(&r);
        if (r.bad || nframes > 4096) ok = false;
    }
    if (ok) {
        frames = vn_calloc(nframes + 1, sizeof(vm_frame));
        for (uint32_t i = 0; ok && i < nframes; i++) {
            frames[i].ret = vn_rd_u32(&r);
            uint32_t n = vn_rd_u32(&r);
            if (r.bad || n > s->nsyms || frames[i].ret >= s->code_size) {
                ok = false;
                break;
            }
            frames[i].saved = vn_calloc(n + 1, sizeof(vm_saved));
            frames[i].cap = n;
            for (uint32_t k = 0; ok && k < n; k++) {
                frames[i].saved[k].sym = vn_rd_u16(&r);
                ok = frames[i].saved[k].sym < s->nsyms && vdeser_value(d, &frames[i].saved[k].old);
                if (ok) frames[i].nsaved = k + 1;
            }
        }
    }
    if (ok) {
        nsyms = vn_rd_u32(&r);
        ok = !r.bad && nsyms == s->nsyms;
    }
    if (ok) {
        vars = vn_calloc(nsyms + 1, sizeof(value));
        for (uint32_t i = 0; ok && i < nsyms; i++) ok = vdeser_value(d, &vars[i]);
    }
    vm_rng rng = e->vm.rng;
    bool has_rng = false;
    if (ok) {
        has_rng = vn_rd_u8(&r) != 0;
        if (has_rng) {
            for (int i = 0; i < 624; i++) rng.mt[i] = vn_rd_u32(&r);
            rng.index = vn_rd_u32(&r);
            if (rng.index > 624) ok = false;
        }
    }
    if (ok) {
        st.clock = vn_rd_f64(&r);
        st.window = vn_rd_u8(&r);
        st.last_speaker = vn_rd_u16(&r);
        st.last_text = get_str(&r);
        st.caption = vn_rd_u8(&r) != 0;
        st.caption_char = vn_rd_u16(&r);
        st.caption_text = get_str(&r);
        st.revert_pending = vn_rd_u8(&r) != 0;
        char *rk = get_str(&r);
        if (rk) {
            snprintf(st.revert_key, sizeof st.revert_key, "%s", rk);
            free(rk);
        }
        st.revert_image = vn_rd_u16(&r);
        nnvl = vn_rd_u32(&r);
        if (r.bad || nnvl > 10000 || st.window > WINDOW_AUTO || st.last_speaker >= s->nchars ||
            st.caption_char >= s->nchars || (st.revert_image != VN_NONE16 && st.revert_image >= s->nimages))
            ok = false;
    }
    if (ok) {
        st.nvl = vn_calloc(nnvl + 1, sizeof(nvl_line));
        st.nnvl = nnvl;
        for (uint32_t i = 0; i < st.nnvl; i++) {
            st.nvl[i].who = get_str(&r);
            st.nvl[i].text = get_str(&r);
            st.nvl[i].color = vn_rd_u32(&r);
        }
        st.voice_sustain = vn_rd_u8(&r) != 0;
        uint32_t nch = vn_rd_u32(&r);
        if (r.bad || nch != s->nchannels) ok = false;
        for (uint32_t i = 0; ok && i < nch && i < MIX_MAX_CHANNELS; i++) {
            chan_state *c = &st.chans[i];
            c->n = vn_rd_u8(&r);
            if (c->n > MIX_PLAYLIST_MAX) {
                ok = false;
                break;
            }
            for (int k = 0; k < c->n; k++) {
                c->assets[k] = vn_rd_u16(&r);
                if (c->assets[k] >= s->nassets) ok = false;
            }
            c->loop = vn_rd_u8(&r) != 0;
            c->volume = vn_rd_f32(&r);
        }
    }
    if (ok) {
        uint32_t n = vn_rd_u32(&r);
        if (r.bad || n > 1000) ok = false;
        for (uint32_t i = 0; ok && i < n; i++) {
            sprite spr;
            memset(&spr, 0, sizeof spr);
            char *key = get_str(&r);
            if (!key) {
                ok = false;
                break;
            }
            snprintf(spr.key, sizeof spr.key, "%s", key);
            free(key);
            spr.image = vn_rd_u16(&r);
            spr.disp = vn_rd_u16(&r);
            spr.nat = vn_rd_u8(&r);
            if (spr.nat > SCENE_AT_MAX) {
                ok = false;
                break;
            }
            for (int k = 0; k < spr.nat; k++) {
                spr.at[k] = vn_rd_u16(&r);
                if (spr.at[k] >= s->ntransforms) ok = false;
            }
            spr.atl = vn_rd_u16(&r);
            spr.zorder = (int32_t)vn_rd_u32(&r);
            spr.shown_at = st.clock - vn_rd_f64(&r);
            if (r.bad || spr.disp >= s->ndisps || (spr.image != VN_NONE16 && spr.image >= s->nimages) ||
                (spr.atl != VN_NONE16 && spr.atl >= s->ntransforms))
                ok = false;
            if (ok) {
                if (st.sc.n == st.sc.cap) {
                    st.sc.cap = st.sc.cap ? st.sc.cap * 2 : 8;
                    st.sc.items = vn_realloc(st.sc.items, sizeof(sprite) * st.sc.cap);
                }
                st.sc.items[st.sc.n++] = spr;
            }
        }
    }
    if (ok && r.pos != r.len) ok = false;

    if (!ok) {
        for (uint32_t i = 0; stack && i < sp; i++) v_dec(stack[i]);
        for (uint32_t i = 0; frames && i < nframes; i++) {
            for (uint32_t k = 0; k < frames[i].nsaved; k++) v_dec(frames[i].saved[k].old);
            free(frames[i].saved);
        }
        for (uint32_t i = 0; vars && i < nsyms; i++) v_dec(vars[i]);
        free(stack);
        free(frames);
        free(vars);
        free_story_strings(&st);
        scene_free(&st.sc);
        vdeser_free(d);
        return false;
    }

    vm *v = &e->vm;
    chan_state before[MIX_MAX_CHANNELS];
    memcpy(before, e->st.chans, sizeof before);
    vm_reset(v);
    for (uint32_t i = 0; i < sp; i++) vm_push(v, stack[i]);
    free(stack);
    if (nframes > v->frames_cap) {
        v->frames_cap = nframes;
        v->frames = vn_realloc(v->frames, sizeof(vm_frame) * v->frames_cap);
    }
    memcpy(v->frames, frames, sizeof(vm_frame) * nframes);
    v->nframes = nframes;
    free(frames);
    for (uint32_t i = 0; i < nsyms; i++) {
        if (s->sym_kind[i] == 1) {
            v_dec(vars[i]);
            continue;
        }
        v_dec(v->vars[i]);
        v->vars[i] = vars[i];
    }
    free(vars);
    if (has_rng) v->rng = rng;
    v->pc = pc;
    v->routine = false;
    vdeser_free(d);

    free_story_strings(&e->st);
    scene_free(&e->st.sc);
    e->st = st;
    e->sctx.clock = st.clock;
    scene_copy(&e->committed, &e->st.sc);
    eng_apply_audio_state(e, before);
    return true;
}

/* ---- save files --------------------------------------------------------------- */

void save_slot_name(int slot, char *out, size_t cap)
{
    if (slot < 0)
        snprintf(out, cap, "%s", slot == -1 ? "quick.sav" : "auto.sav");
    else
        snprintf(out, cap, "slot-%02d.sav", slot + 1);
}

void thumb_capture(vn_engine *e)
{
    uint8_t *rgb = vn_alloc((size_t)GFX_W * GFX_H * 3);
    e->thumb_valid = gfx_capture_rgb(rgb);
    if (e->thumb_valid) {
        for (int y = 0; y < THUMB_H; y++) {
            for (int x = 0; x < THUMB_W; x++) {
                unsigned r = 0, g = 0, b = 0;
                for (int dy = 0; dy < 4; dy++)
                    for (int dx = 0; dx < 4; dx++) {
                        const uint8_t *p = rgb + 3 * ((size_t)(y * 4 + dy) * GFX_W + (size_t)(x * 4 + dx));
                        r += p[0];
                        g += p[1];
                        b += p[2];
                    }
                r /= 16;
                g /= 16;
                b /= 16;
                e->thumb[y * THUMB_W + x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
        }
    }
    free(rgb);
}

bool save_write(vn_engine *e, const char *name)
{
    if (!e->current.data) return false;
    vn_buf b = {0};
    vn_buf_put(&b, "VNSV", 4);
    vn_buf_u16(&b, SAVE_VERSION);
    vn_buf_u16(&b, 0);
    vn_buf_u64(&b, e->script.hash);
    vn_buf_u64(&b, e->cfg.deterministic ? 0 : (uint64_t)time(NULL));
    vn_buf_u16(&b, THUMB_W);
    vn_buf_u16(&b, THUMB_H);
    for (int i = 0; i < THUMB_W * THUMB_H; i++) vn_buf_u16(&b, e->thumb_valid ? e->thumb[i] : 0);
    char desc[64] = {0};
    if (e->st.last_text) {
        vn_buf plain = {0};
        text_plain(e->st.last_text, strlen(e->st.last_text), &plain);
        size_t n = plain.len < sizeof desc - 4 ? plain.len : sizeof desc - 4;
        while (n && n < plain.len && (plain.data[n] & 0xC0) == 0x80) n--;   /* whole UTF-8 characters */
        memcpy(desc, plain.data, n);
        for (size_t i = 0; i < n; i++)
            if (desc[i] == '\n') desc[i] = ' ';
        vn_buf_free(&plain);
    }
    put_str(&b, desc);
    vn_buf_u32(&b, (uint32_t)e->history_n);
    for (int i = 0; i < e->history_n; i++) {
        put_str(&b, e->history[i].who);
        put_str(&b, e->history[i].text);
    }
    vn_buf_u32(&b, (uint32_t)e->current.len);
    vn_buf_put(&b, e->current.data, e->current.len);
    vn_buf_u32(&b, vn_crc32(0, b.data, b.len));
    char path[512];
    vn_path_join(path, sizeof path, e->save_dir, name);
    bool ok = vn_fs_write_atomic(path, b.data, b.len);
    vn_buf_free(&b);
    return ok;
}

bool save_read(vn_engine *e, const char *name, bool apply, struct slot_info *info)
{
    char path[512];
    vn_path_join(path, sizeof path, e->save_dir, name);
    if (info) memset(info, 0, sizeof *info);
    void *data;
    size_t size;
    if (!vn_fs_read_all(path, &data, &size)) return false;
    if (info) info->used = true;
    bool ok = size > 8 && vn_le32((const uint8_t *)data + size - 4) == vn_crc32(0, data, size - 4);
    vn_rd r;
    vn_rd_init(&r, data, ok ? size - 4 : 0);
    const uint8_t *magic = vn_rd_bytes(&r, 4);
    ok = ok && magic && memcmp(magic, "VNSV", 4) == 0 && vn_rd_u16(&r) == SAVE_VERSION;
    vn_rd_u16(&r);
    uint64_t hash = vn_rd_u64(&r);
    uint64_t when = vn_rd_u64(&r);
    uint16_t tw = vn_rd_u16(&r), th = vn_rd_u16(&r);
    ok = ok && !r.bad && tw == THUMB_W && th == THUMB_H;
    const uint8_t *thumb = ok ? vn_rd_bytes(&r, (size_t)tw * th * 2) : NULL;
    char *desc = ok ? get_str(&r) : NULL;
    ok = ok && thumb && !r.bad;
    bool script_ok = ok && hash == e->script.hash;
    if (info && ok) {
        info->valid = script_ok;
        info->time = when;
        snprintf(info->desc, sizeof info->desc, "%s", desc ? desc : "");
        for (int i = 0; i < THUMB_W * THUMB_H; i++) info->thumb[i] = vn_le16(thumb + 2 * i);
    }
    free(desc);
    if (!apply || !ok) {
        free(data);
        if (apply && !ok) eng_message(e, "This save file is damaged.");
        return ok;
    }
    if (!script_ok) {
        free(data);
        eng_message(e, "This save was made with a different build of the game.");
        return false;
    }
    uint32_t nh = vn_rd_u32(&r);
    if (r.bad || nh > HISTORY_MAX) {
        free(data);
        eng_message(e, "This save file is damaged.");
        return false;
    }
    char *who[HISTORY_MAX], *text[HISTORY_MAX];
    for (uint32_t i = 0; i < nh; i++) {
        who[i] = get_str(&r);
        text[i] = get_str(&r);
    }
    uint32_t cklen = vn_rd_u32(&r);
    const uint8_t *ck = vn_rd_bytes(&r, cklen);
    bool restored = ck && !r.bad && ck_restore(e, ck, cklen);
    if (restored) {
        history_free(e);
        for (uint32_t i = 0; i < nh; i++) {
            e->history[i].who = who[i];
            e->history[i].text = text[i] ? text[i] : vn_strdup("");
            e->history[i].serial = 0;
        }
        e->history_n = (int)nh;
        rb_clear(e);
        free(e->current.data);
        e->current.data = vn_alloc(cklen);
        memcpy(e->current.data, ck, cklen);
        e->current.len = cklen;
    } else {
        for (uint32_t i = 0; i < nh; i++) {
            free(who[i]);
            free(text[i]);
        }
        eng_message(e, "This save file is damaged.");
    }
    free(data);
    return restored;
}

/* ---- persistent data ---------------------------------------------------------- */

#define PERSISTENT_FILE "persistent.bin"

static void persistent_blob(vn_engine *e, vn_buf *b)
{
    const vn_script *s = &e->script;
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->nsyms; i++)
        if (s->sym_kind[i] == 1 && e->vm.vars[i].t != V_UNSET) n++;
    vn_buf_put(b, "VNPS", 4);
    vn_buf_u32(b, n);
    vser *ser = vser_new(b);
    for (uint32_t i = 0; i < s->nsyms; i++) {
        if (s->sym_kind[i] != 1 || e->vm.vars[i].t == V_UNSET) continue;
        put_str(b, vn_str(s, s->sym_name[i]));
        vser_value(ser, e->vm.vars[i]);
    }
    vser_free(ser);
}

void persistent_save(vn_engine *e, bool force)
{
    vn_buf b = {0};
    persistent_blob(e, &b);
    uint64_t h = vn_fnv1a64(b.data, b.len);
    if (force || h != e->persistent_hash) {
        vn_buf_u32(&b, vn_crc32(0, b.data, b.len));
        char path[512];
        vn_path_join(path, sizeof path, e->save_dir, PERSISTENT_FILE);
        if (vn_fs_write_atomic(path, b.data, b.len))
            e->persistent_hash = h;
        else
            vn_log("save: writing persistent data failed");
    }
    vn_buf_free(&b);
}

void persistent_load(vn_engine *e)
{
    char path[512];
    vn_path_join(path, sizeof path, e->save_dir, PERSISTENT_FILE);
    void *data;
    size_t size;
    if (!vn_fs_read_all(path, &data, &size)) return;
    if (size < 12 || vn_le32((const uint8_t *)data + size - 4) != vn_crc32(0, data, size - 4) ||
        memcmp(data, "VNPS", 4) != 0) {
        vn_log("save: %s is damaged; ignoring it", path);
        free(data);
        return;
    }
    vn_rd r;
    vn_rd_init(&r, data, size - 4);
    vn_rd_bytes(&r, 4);
    uint32_t n = vn_rd_u32(&r);
    vdeser *d = vdeser_new(&r);
    for (uint32_t i = 0; i < n && !r.bad; i++) {
        char *name = get_str(&r);
        value v;
        if (!name || !vdeser_value(d, &v)) {
            free(name);
            break;
        }
        int32_t sym = vm_find_sym(&e->vm, name, strlen(name));
        if (sym >= 0 && e->script.sym_kind[sym] == 1) {
            v_dec(e->vm.vars[sym]);
            e->vm.vars[sym] = v;
        } else {
            v_dec(v);   /* the variable no longer exists in this build */
        }
        free(name);
    }
    vdeser_free(d);
    free(data);
    vn_buf b = {0};
    persistent_blob(e, &b);
    e->persistent_hash = vn_fnv1a64(b.data, b.len);
    vn_buf_free(&b);
}

/* ---- history ----------------------------------------------------------------- */

void history_add(vn_engine *e, const char *who, const char *text)
{
    if (e->history_n == HISTORY_MAX) {
        free(e->history[0].who);
        free(e->history[0].text);
        memmove(e->history, e->history + 1, sizeof(history_entry) * (HISTORY_MAX - 1));
        e->history_n--;
    }
    history_entry *h = &e->history[e->history_n++];
    h->who = who ? vn_strdup(who) : NULL;
    h->text = vn_strdup(text);
    h->serial = e->current.serial;
}

void history_truncate(vn_engine *e, uint32_t serial)
{
    while (e->history_n && e->history[e->history_n - 1].serial >= serial && e->history[e->history_n - 1].serial) {
        e->history_n--;
        free(e->history[e->history_n].who);
        free(e->history[e->history_n].text);
    }
}

void history_free(vn_engine *e)
{
    for (int i = 0; i < e->history_n; i++) {
        free(e->history[i].who);
        free(e->history[i].text);
    }
    e->history_n = 0;
}
