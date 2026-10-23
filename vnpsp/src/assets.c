#include "assets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __PSP__
#include <malloc.h>
#endif

void *vn_aligned_alloc(size_t size)
{
#ifdef __PSP__
    /* The GE reads textures and CLUTs from 16-byte aligned addresses. */
    void *p = memalign(16, size ? size : 16);
    if (!p) {
        vn_log("vnpsp: out of memory (%zu bytes)", size);
        abort();
    }
    return p;
#else
    return vn_alloc(size);
#endif
}

void vn_aligned_free(void *p)
{
    free(p);
}

static bool err_out(char *err, size_t cap, const char *msg)
{
    snprintf(err, cap, "data.pak: %s", msg);
    return false;
}

static const char *entry_name(const vn_assets *a, uint32_t i)
{
    return a->names + a->entries[i].name;
}

static int32_t find_entry(const vn_assets *a, const char *key)
{
    for (uint32_t i = 0; i < a->count; i++)
        if (strcmp(entry_name(a, i), key) == 0) return (int32_t)i;
    return -1;
}

static bool read_entry(vn_assets *a, uint32_t index, uint8_t **out, uint32_t *size, bool aligned)
{
    vn_pack_entry *e = &a->entries[index];
    uint8_t *buf = aligned ? vn_aligned_alloc(e->size + 1) : vn_alloc(e->size + 1);
    if (!vn_fs_read_at(a->file, e->offset, buf, e->size)) {
        vn_log("assets: reading %s failed", entry_name(a, index));
        if (aligned)
            vn_aligned_free(buf);
        else
            free(buf);
        return false;
    }
    a->bytes_read += e->size;
    a->loads++;
    *out = buf;
    *size = e->size;
    return true;
}

/* ---- fonts ------------------------------------------------------------------- */

static void font_free(vn_font *f)
{
    if (!f) return;
    for (uint8_t i = 0; i < f->npages; i++) gfx_tex_destroy(f->pages[i]);
    free(f->pages);
    vn_aligned_free(f->blob);
    free(f);
}

static vn_font *font_load(vn_assets *a, uint32_t index)
{
    uint8_t *blob;
    uint32_t size;
    if (!read_entry(a, index, &blob, &size, true)) return NULL;
    vn_rd r;
    vn_rd_init(&r, blob, size);
    const uint8_t *magic = vn_rd_bytes(&r, 4);
    vn_font *f = vn_calloc(1, sizeof *f);
    f->blob = blob;
    if (!magic || memcmp(magic, "FNT1", 4) != 0) goto bad;
    f->size = vn_rd_u16(&r);
    f->ascent = (int16_t)vn_rd_u16(&r);
    f->descent = (int16_t)vn_rd_u16(&r);
    f->line_h = (int16_t)vn_rd_u16(&r);
    f->fmt = vn_rd_u8(&r);
    f->npages = vn_rd_u8(&r);
    f->nglyphs = vn_rd_u16(&r);
    f->outline = vn_rd_u8(&r);
    vn_rd_bytes(&r, 3);
    if (r.bad || (f->fmt != GFX_T4 && f->fmt != GFX_T8) || f->npages == 0 || f->line_h <= 0) goto bad;
    f->pages = vn_calloc(f->npages, sizeof *f->pages);
    uint16_t *rows = vn_calloc(f->npages, sizeof *rows);
    for (uint8_t i = 0; i < f->npages; i++) {
        uint16_t width = vn_rd_u16(&r);
        rows[i] = vn_rd_u16(&r);
        uint32_t off = vn_rd_u32(&r), len = vn_rd_u32(&r);
        size_t need = (size_t)rows[i] * (f->fmt == GFX_T4 ? 256u : 512u);
        if (r.bad || width != 512 || rows[i] == 0 || rows[i] > 512 || rows[i] % 8 || off % 16 || off > size ||
            len > size - off || len < need) {
            free(rows);
            goto bad;
        }
        uint8_t th = 0;
        while ((1u << th) < rows[i]) th++;
        gfx_tex_desc d = {f->fmt, true, 512, rows[i], 9, th, 512, blob + off, len};
        f->pages[i] = gfx_tex_create(&d);
        if (!f->pages[i]) {
            free(rows);
            goto bad;
        }
    }
    f->glyphs = vn_calloc(f->nglyphs ? f->nglyphs : 1, sizeof *f->glyphs);
    for (uint32_t i = 0; i < f->nglyphs; i++) {
        vn_glyph *g = &f->glyphs[i];
        g->cp = vn_rd_u32(&r);
        g->x = vn_rd_u16(&r);
        g->y = vn_rd_u16(&r);
        g->w = vn_rd_u16(&r);
        g->h = vn_rd_u16(&r);
        g->bx = (int16_t)vn_rd_u16(&r);
        g->by = (int16_t)vn_rd_u16(&r);
        g->adv64 = vn_rd_u16(&r);
        g->page = vn_rd_u8(&r);
        vn_rd_u8(&r);
        if (r.bad || g->page >= f->npages || g->x + g->w > 512 || g->y + g->h > rows[g->page] ||
            (i && g->cp <= f->glyphs[i - 1].cp)) {
            free(rows);
            goto bad;
        }
    }
    free(rows);
    return f;
bad:
    vn_log("assets: font entry %s is malformed", entry_name(a, index));
    font_free(f);
    return NULL;
}

const vn_glyph *vn_font_glyph(const vn_font *f, uint32_t cp)
{
    size_t lo = 0, hi = f->nglyphs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (f->glyphs[mid].cp == cp) return &f->glyphs[mid];
        if (f->glyphs[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

const vn_font *vn_assets_font(const vn_assets *a, int role)
{
    if (role >= 0 && role < FONT_ROLES && a->fonts[role]) return a->fonts[role];
    return a->fonts[FONT_TEXT];
}

const vn_font *vn_assets_face(const vn_assets *a, const char *name, size_t len)
{
    for (uint32_t i = 0; i < a->nfaces; i++)
        if (strlen(a->face_names[i]) == len && memcmp(a->face_names[i], name, len) == 0) return a->faces[i];
    return NULL;
}

/* ---- textures ------------------------------------------------------------------ */

static void texture_free(vn_texture *t)
{
    if (!t) return;
    for (uint8_t i = 0; i < t->ntiles; i++) gfx_tex_destroy(t->tiles[i].tex);
    free(t->tiles);
    vn_aligned_free(t->blob);
    free(t);
}

static vn_texture *texture_parse(uint8_t *blob, uint32_t size, uint16_t asset)
{
    vn_texture *t = vn_calloc(1, sizeof *t);
    t->blob = blob;
    t->asset = asset;
    t->bytes = size;
    if (size < 16 || memcmp(blob, "TEX1", 4) != 0) goto bad;
    t->w = vn_le16(blob + 4);
    t->h = vn_le16(blob + 6);
    t->src_w = vn_le16(blob + 8);
    t->src_h = vn_le16(blob + 10);
    t->fmt = blob[12];
    t->ntiles = blob[13];
    if ((t->fmt != GFX_DXT1 && t->fmt != GFX_DXT5) || t->ntiles == 0 || 16 + 24u * t->ntiles > size) goto bad;
    t->tiles = vn_calloc(t->ntiles, sizeof *t->tiles);
    for (uint8_t i = 0; i < t->ntiles; i++) {
        const uint8_t *p = blob + 16 + 24 * i;
        vn_tile *tl = &t->tiles[i];
        tl->x = vn_le16(p);
        tl->y = vn_le16(p + 2);
        tl->w = vn_le16(p + 4);
        tl->h = vn_le16(p + 6);
        uint8_t lw = p[8], lh = p[9];
        uint16_t bufw = vn_le16(p + 10), rows = vn_le16(p + 12);
        uint32_t off = vn_le32(p + 16), len = vn_le32(p + 20);
        size_t need = (size_t)(bufw / 4) * (rows / 4) * (t->fmt == GFX_DXT1 ? 8u : 16u);
        if (lw > 9 || lh > 9 || tl->w == 0 || tl->h == 0 || tl->w > (1u << lw) || tl->h > (1u << lh) ||
            bufw < tl->w || bufw % 4 || rows % 4 || rows < tl->h || off % 16 || off > size || len > size - off ||
            len < need || tl->x + tl->w > t->w || tl->y + tl->h > t->h)
            goto bad;
        gfx_tex_desc d = {t->fmt, false, tl->w, rows, lw, lh, bufw, blob + off, len};
        tl->tex = gfx_tex_create(&d);
        if (!tl->tex) goto bad;
    }
    return t;
bad:
    texture_free(t);
    return NULL;
}

static void evict_until(vn_assets *a, size_t need)
{
    while (a->used + need > a->budget) {
        uint32_t victim = VN_NONE32;
        uint64_t oldest = UINT64_MAX;
        for (uint32_t i = 0; i < a->script->nassets; i++) {
            vn_texture *t = a->textures[i];
            if (t && t->last_used < a->frame && t->last_used < oldest) {
                oldest = t->last_used;
                victim = i;
            }
        }
        if (victim == VN_NONE32) return;   /* everything left is in use this frame */
        a->used -= a->textures[victim]->bytes;
        texture_free(a->textures[victim]);
        a->textures[victim] = NULL;
    }
}

vn_texture *vn_assets_texture(vn_assets *a, uint16_t asset)
{
    if (asset >= a->script->nassets) return NULL;
    vn_texture *t = a->textures[asset];
    if (t) {
        t->last_used = a->frame;
        return t;
    }
    int32_t e = a->asset_entry[asset];
    if (e < 0 || a->entries[e].kind != ASSET_IMAGE) return NULL;
    evict_until(a, a->entries[e].size);
    uint8_t *blob;
    uint32_t size;
    if (!read_entry(a, (uint32_t)e, &blob, &size, true)) return NULL;
    t = texture_parse(blob, size, asset);
    if (!t) {
        vn_log("assets: image entry %s is malformed", entry_name(a, (uint32_t)e));
        /* Remember the failure so a broken entry is not re-read every frame. */
        a->asset_entry[asset] = -1;
        return NULL;
    }
    t->last_used = a->frame;
    a->textures[asset] = t;
    a->used += t->bytes;
    return t;
}

void vn_assets_new_frame(vn_assets *a)
{
    a->frame++;
}

uint64_t vn_assets_prefetch_label(vn_assets *a, uint32_t addr)
{
    const vn_script *s = a->script;
    uint64_t before = a->bytes_read;
    for (uint32_t i = 0; i < s->nlabels; i++) {
        if (s->labels[i].addr != addr || a->label_scene[i] < 0) continue;
        uint32_t sc = (uint32_t)a->label_scene[i];
        for (uint32_t k = 0; k < a->scene_count[sc]; k++) {
            uint32_t entry = a->scene_list[a->scene_first[sc] + k];
            if (a->entries[entry].kind != ASSET_IMAGE) continue;
            for (uint32_t as = 0; as < s->nassets; as++)
                if (a->asset_entry[as] == (int32_t)entry) {
                    vn_texture *t = vn_assets_texture(a, (uint16_t)as);
                    /* Prefetched images count as used now so later loads in this scene keep them. */
                    if (t) t->last_used = a->frame;
                }
        }
        return a->bytes_read - before;
    }
    return 0;
}

bool vn_assets_read(vn_assets *a, uint16_t asset, uint8_t **data, uint32_t *size)
{
    if (asset >= a->script->nassets || a->asset_entry[asset] < 0) return false;
    return read_entry(a, (uint32_t)a->asset_entry[asset], data, size, false);
}

void vn_texture_bounds(vn_texture *t)
{
    if (t->bounds_known) return;
    t->bounds_known = true;
    if (t->fmt == GFX_DXT1) {
        t->opaque_x0 = t->opaque_y0 = 0;
        t->opaque_x1 = t->w;
        t->opaque_y1 = t->h;
        return;
    }
    uint32_t x0 = UINT32_MAX, y0 = UINT32_MAX, x1 = 0, y1 = 0;
    for (uint8_t i = 0; i < t->ntiles; i++) {
        const uint8_t *p = t->blob + 16 + 24 * i;
        uint16_t bufw = vn_le16(p + 10), rows = vn_le16(p + 12);
        const uint8_t *data = t->blob + vn_le32(p + 16);
        vn_tile *tl = &t->tiles[i];
        for (uint32_t by = 0; by < rows / 4u; by++) {
            for (uint32_t bx = 0; bx < bufw / 4u; bx++) {
                const uint8_t *b = data + ((size_t)by * (bufw / 4) + bx) * 16;
                uint8_t a0 = b[14], a1 = b[15];
                uint64_t bits = 0;
                for (int k = 0; k < 6; k++) bits |= (uint64_t)b[8 + k] << (8 * k);
                bool visible = false;
                for (int px = 0; px < 16 && !visible; px++) {
                    unsigned idx = (unsigned)(bits >> (3 * px)) & 7;
                    if (idx == 0) visible = a0 > 0;
                    else if (idx == 1) visible = a1 > 0;
                    else if (a0 > a1) visible = true;
                    else if (idx == 6) visible = false;
                    else if (idx == 7) visible = true;
                    else visible = (a0 | a1) != 0;
                }
                if (!visible) continue;
                uint32_t px0 = tl->x + bx * 4, py0 = tl->y + by * 4;
                if (px0 >= (uint32_t)tl->x + tl->w || py0 >= (uint32_t)tl->y + tl->h) continue;
                if (px0 < x0) x0 = px0;
                if (py0 < y0) y0 = py0;
                if (px0 + 4 > x1) x1 = px0 + 4;
                if (py0 + 4 > y1) y1 = py0 + 4;
            }
        }
    }
    if (x0 == UINT32_MAX) x0 = y0 = x1 = y1 = 0;
    t->opaque_x0 = (uint16_t)x0;
    t->opaque_y0 = (uint16_t)y0;
    t->opaque_x1 = (uint16_t)(x1 > t->w ? t->w : x1);
    t->opaque_y1 = (uint16_t)(y1 > t->h ? t->h : y1);
}

/* ---- open/close ---------------------------------------------------------------- */

bool vn_assets_open(vn_assets *a, const char *path, const vn_script *s, size_t budget, char *err, size_t errcap)
{
    memset(a, 0, sizeof *a);
    a->script = s;
    a->budget = budget;
    a->file = vn_fs_open(path);
    if (!a->file) return err_out(err, errcap, "cannot open");
    uint8_t head[32];
    if (!vn_fs_read_at(a->file, 0, head, sizeof head) || memcmp(head, "VNPK", 4) != 0 || vn_le16(head + 4) != 1)
        return err_out(err, errcap, "not a VNPK version 1 file");
    uint64_t fsize = vn_fs_size(a->file);
    a->count = vn_le32(head + 8);
    uint32_t index_off = vn_le32(head + 12), scene_off = vn_le32(head + 16);
    a->nscenes = vn_le32(head + 20);
    uint32_t names_off = vn_le32(head + 24);
    a->names_len = vn_le32(head + 28);
    if (a->count > 1000000 || a->nscenes > 100000 || index_off > fsize || 16ull * a->count > fsize - index_off ||
        scene_off > fsize || 12ull * a->nscenes > fsize - scene_off || names_off > fsize ||
        a->names_len > fsize - names_off || a->names_len == 0)
        return err_out(err, errcap, "bad header");

    uint8_t *idx = vn_alloc(16 * (size_t)a->count + 1);
    a->names = vn_alloc(a->names_len + 1);
    bool ok = vn_fs_read_at(a->file, index_off, idx, 16 * (size_t)a->count) &&
              vn_fs_read_at(a->file, names_off, a->names, a->names_len);
    a->names[a->names_len] = '\0';
    if (!ok || a->names[a->names_len - 1] != '\0') {
        free(idx);
        return err_out(err, errcap, "cannot read index");
    }
    a->entries = vn_calloc(a->count ? a->count : 1, sizeof *a->entries);
    for (uint32_t i = 0; i < a->count; i++) {
        const uint8_t *p = idx + 16 * (size_t)i;
        vn_pack_entry *e = &a->entries[i];
        e->name = vn_le32(p);
        e->kind = p[4];
        e->offset = vn_le32(p + 8);
        e->size = vn_le32(p + 12);
        if (e->name >= a->names_len || e->offset > fsize || e->size > fsize - e->offset || e->kind > ASSET_MOVIE) {
            free(idx);
            return err_out(err, errcap, "bad index entry");
        }
    }
    free(idx);

    uint8_t *sc = vn_alloc(12 * (size_t)a->nscenes + 1);
    if (!vn_fs_read_at(a->file, scene_off, sc, 12 * (size_t)a->nscenes)) {
        free(sc);
        return err_out(err, errcap, "cannot read scenes");
    }
    a->scene_name = vn_calloc(a->nscenes + 1, sizeof(uint32_t));
    a->scene_first = vn_calloc(a->nscenes + 1, sizeof(uint32_t));
    a->scene_count = vn_calloc(a->nscenes + 1, sizeof(uint32_t));
    uint64_t list_len = 0;
    for (uint32_t i = 0; i < a->nscenes; i++) {
        a->scene_name[i] = vn_le32(sc + 12 * i);
        a->scene_first[i] = vn_le32(sc + 12 * i + 4);
        a->scene_count[i] = vn_le32(sc + 12 * i + 8);
        if (a->scene_name[i] >= a->names_len) {
            free(sc);
            return err_out(err, errcap, "bad scene name");
        }
        uint64_t end = (uint64_t)a->scene_first[i] + a->scene_count[i];
        if (end > list_len) list_len = end;
    }
    free(sc);
    uint64_t list_off = scene_off + 12ull * a->nscenes;
    if (list_len * 4 > fsize - list_off) return err_out(err, errcap, "bad scene list");
    a->scene_list_len = (uint32_t)list_len;
    a->scene_list = vn_calloc(list_len + 1, sizeof(uint32_t));
    uint8_t *raw = vn_alloc(4 * (size_t)list_len + 1);
    if (!vn_fs_read_at(a->file, list_off, raw, 4 * (size_t)list_len)) {
        free(raw);
        return err_out(err, errcap, "cannot read scene list");
    }
    for (uint32_t i = 0; i < list_len; i++) {
        a->scene_list[i] = vn_le32(raw + 4 * (size_t)i);
        if (a->scene_list[i] >= a->count) {
            free(raw);
            return err_out(err, errcap, "bad scene list entry");
        }
    }
    free(raw);

    a->asset_entry = vn_calloc(s->nassets + 1, sizeof(int32_t));
    a->textures = vn_calloc(s->nassets + 1, sizeof(vn_texture *));
    uint32_t missing = 0;
    for (uint32_t i = 0; i < s->nassets; i++) {
        a->asset_entry[i] = find_entry(a, vn_str(s, s->assets[i].key));
        if (a->asset_entry[i] < 0) missing++;
    }
    if (missing) vn_log("assets: %u assets named by the script are missing from the pack", missing);
    a->label_scene = vn_calloc(s->nlabels + 1, sizeof(int32_t));
    for (uint32_t i = 0; i < s->nlabels; i++) {
        a->label_scene[i] = -1;
        for (uint32_t k = 0; k < a->nscenes; k++)
            if (strcmp(a->names + a->scene_name[k], vn_str(s, s->labels[i].name)) == 0) a->label_scene[i] = (int32_t)k;
    }

    static const char *const roles[FONT_ROLES] = {"font/text", "font/name", "font/interface"};
    for (int r = 0; r < FONT_ROLES; r++) {
        int32_t e = find_entry(a, roles[r]);
        if (e >= 0) a->fonts[r] = font_load(a, (uint32_t)e);
    }
    if (!a->fonts[FONT_TEXT]) return err_out(err, errcap, "font/text is missing or malformed");
    for (uint32_t i = 0; i < a->count; i++) {
        const char *n = entry_name(a, i);
        if (a->entries[i].kind != ASSET_FONT || strncmp(n, "font/face/", 10) != 0) continue;
        vn_font *f = font_load(a, i);
        if (!f) continue;
        a->faces = vn_realloc(a->faces, sizeof *a->faces * (a->nfaces + 1));
        a->face_names = vn_realloc(a->face_names, sizeof *a->face_names * (a->nfaces + 1));
        a->faces[a->nfaces] = f;
        a->face_names[a->nfaces] = vn_strdup(n + 10);
        a->nfaces++;
    }
    return true;
}

void vn_assets_close(vn_assets *a)
{
    if (a->textures)
        for (uint32_t i = 0; i < a->script->nassets; i++) texture_free(a->textures[i]);
    for (int r = 0; r < FONT_ROLES; r++) {
        bool shared = false;
        for (int k = 0; k < r; k++) shared |= a->fonts[k] == a->fonts[r];
        if (!shared) font_free(a->fonts[r]);
    }
    for (uint32_t i = 0; i < a->nfaces; i++) {
        font_free(a->faces[i]);
        free(a->face_names[i]);
    }
    free(a->faces);
    free(a->face_names);
    free(a->textures);
    free(a->asset_entry);
    free(a->label_scene);
    free(a->entries);
    free(a->names);
    free(a->scene_name);
    free(a->scene_first);
    free(a->scene_count);
    free(a->scene_list);
    vn_fs_close(a->file);
    memset(a, 0, sizeof *a);
}
