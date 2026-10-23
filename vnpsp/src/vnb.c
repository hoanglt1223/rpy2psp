#include "vnb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Operand formats, indexed by opcode (mirrors OPCODES in bytecode.py). */
static const char *const op_formats[OP_COUNT] = {
    "", "", "", "", "i", "d", "s", "v", "v", "v", "", "",
    "h", "h", "h", "h", "", "", "", "b", "b", "b",
    "a", "a", "a", "a", "a", "bb", "bb", "a", "", "", "", "v",
    "hshb", "hhb", "hbS", "M", "", "h", "h", "h", "h", "h", "h", "bf", "h", "", "bh", "bh", "h",
};

typedef struct section {
    const uint8_t *p;
    uint32_t size;
} section;

static bool find_section(const uint8_t *data, size_t size, const char id[4], section *out)
{
    uint32_t count = vn_le32(data + 8);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = data + 12 + 12 * (size_t)i;
        if (memcmp(e, id, 4) != 0) continue;
        uint32_t off = vn_le32(e + 4), len = vn_le32(e + 8);
        if (off > size || len > size - off) return false;
        out->p = data + off;
        out->size = len;
        return true;
    }
    out->p = NULL;
    out->size = 0;
    return false;
}

static bool fail(char *err, size_t cap, const char *msg)
{
    snprintf(err, cap, "script.vnb: %s", msg);
    return false;
}

static bool load_strings(vn_script *s, section sec)
{
    vn_rd r;
    vn_rd_init(&r, sec.p, sec.size);
    uint32_t n = vn_rd_u32(&r);
    if (r.bad || n > sec.size / 4) return false;
    const uint8_t *offs = vn_rd_bytes(&r, (size_t)n * 4);
    if (!offs) return false;
    size_t base = r.pos;
    s->nstrings = n;
    s->strings = vn_calloc(n, sizeof *s->strings);
    s->string_len = vn_calloc(n, sizeof *s->string_len);
    for (uint32_t i = 0; i < n; i++) {
        size_t o = base + vn_le32(offs + 4 * (size_t)i);
        if (o >= sec.size) return false;
        const uint8_t *end = memchr(sec.p + o, 0, sec.size - o);
        if (!end) return false;
        s->strings[i] = (const char *)(sec.p + o);
        s->string_len[i] = (uint32_t)(end - (sec.p + o));
    }
    return true;
}

static bool str_ok(const vn_script *s, uint32_t id)
{
    return id == VN_NONE32 || id < s->nstrings;
}

static bool split_image_name(vn_image *im, const char *name)
{
    im->words = vn_strdup(name);
    size_t n = 0;
    for (char *p = im->words; *p; p++)
        if (*p == ' ') n++;
    im->attrs = vn_calloc(n + 1, sizeof *im->attrs);
    char *save = im->words;
    im->tag = NULL;
    im->nattrs = 0;
    for (char *p = im->words;; p++) {
        if (*p == ' ' || *p == '\0') {
            bool end = *p == '\0';
            *p = '\0';
            if (*save) {
                if (!im->tag)
                    im->tag = save;
                else if (im->nattrs < 255)
                    im->attrs[im->nattrs++] = save;
            }
            if (end) break;
            save = p + 1;
        }
    }
    return im->tag != NULL;
}

bool vn_script_load(vn_script *s, uint8_t *data, size_t size, char *err, size_t errcap)
{
    memset(s, 0, sizeof *s);
    s->file = data;
    s->size = size;
    s->title = s->version = VN_NONE32;
    if (size < 12 || memcmp(data, "VNB1", 4) != 0) return fail(err, errcap, "not a VNB1 file");
    if (vn_le16(data + 4) != 2) return fail(err, errcap, "unsupported version (recompile the game)");
    uint32_t nsec = vn_le32(data + 8);
    if (nsec > 64 || 12 + 12 * (size_t)nsec > size) return fail(err, errcap, "bad section table");
    s->hash = vn_fnv1a64(data, size);

    section sec;
    if (!find_section(data, size, "STRS", &sec) || !load_strings(s, sec)) return fail(err, errcap, "bad STRS");

    if (!find_section(data, size, "HEAD", &sec) || sec.size < 24) return fail(err, errcap, "bad HEAD");
    s->init_addr = vn_le32(sec.p);
    s->defaults_addr = vn_le32(sec.p + 4);
    s->start_addr = vn_le32(sec.p + 8);
    s->src_w = vn_lef32(sec.p + 16);
    s->src_h = vn_lef32(sec.p + 20);
    if (!(s->src_w >= 1.0f && s->src_w <= 16384.0f && s->src_h >= 1.0f && s->src_h <= 16384.0f))
        return fail(err, errcap, "bad screen size");

    if (!find_section(data, size, "CODE", &sec) || sec.size == 0) return fail(err, errcap, "bad CODE");
    s->code = sec.p;
    s->code_size = sec.size;
    if (s->init_addr >= s->code_size || s->defaults_addr >= s->code_size || s->start_addr >= s->code_size)
        return fail(err, errcap, "entry point outside CODE");

    vn_rd r;
#define SECTION(id)                                                         \
    if (!find_section(data, size, id, &sec)) return fail(err, errcap, "missing " id); \
    vn_rd_init(&r, sec.p, sec.size)
#define CHECK_RD(what) \
    if (r.bad) return fail(err, errcap, "truncated " what)

    SECTION("SYMS");
    s->nsyms = vn_rd_u32(&r);
    if (s->nsyms > sec.size / 8 || s->nsyms > 0xFFFF) return fail(err, errcap, "bad SYMS");
    s->sym_name = vn_calloc(s->nsyms, sizeof *s->sym_name);
    s->sym_kind = vn_calloc(s->nsyms, 1);
    for (uint32_t i = 0; i < s->nsyms; i++) {
        s->sym_name[i] = vn_rd_u32(&r);
        s->sym_kind[i] = vn_rd_u8(&r);
        vn_rd_bytes(&r, 3);
        if (s->sym_name[i] >= s->nstrings) return fail(err, errcap, "bad symbol name");
    }
    CHECK_RD("SYMS");

    SECTION("LABL");
    s->nlabels = vn_rd_u32(&r);
    if (s->nlabels > sec.size / 8) return fail(err, errcap, "bad LABL");
    s->labels = vn_calloc(s->nlabels, sizeof *s->labels);
    for (uint32_t i = 0; i < s->nlabels; i++) {
        s->labels[i].name = vn_rd_u32(&r);
        s->labels[i].addr = vn_rd_u32(&r);
        if (s->labels[i].name >= s->nstrings || s->labels[i].addr >= s->code_size)
            return fail(err, errcap, "bad label");
    }
    CHECK_RD("LABL");

    SECTION("CHRS");
    s->nchars = vn_rd_u32(&r);
    if (s->nchars > sec.size / 38 || s->nchars < 5) return fail(err, errcap, "bad CHRS");
    s->chars = vn_calloc(s->nchars, sizeof *s->chars);
    for (uint32_t i = 0; i < s->nchars; i++) {
        vn_char *c = &s->chars[i];
        c->name = vn_rd_u32(&r);
        const uint8_t *who = vn_rd_bytes(&r, 4), *what = vn_rd_bytes(&r, 4);
        if (who && what) {
            memcpy(c->who_color, who, 4);
            memcpy(c->what_color, what, 4);
        }
        c->kind = vn_rd_u8(&r);
        c->flags = vn_rd_u8(&r);
        c->image = vn_rd_u32(&r);
        c->side_image = vn_rd_u16(&r);
        c->what_prefix = vn_rd_u32(&r);
        c->what_suffix = vn_rd_u32(&r);
        c->who_prefix = vn_rd_u32(&r);
        c->who_suffix = vn_rd_u32(&r);
        c->ctc = vn_rd_u16(&r);
        if (!str_ok(s, c->name) || !str_ok(s, c->image) || !str_ok(s, c->what_prefix) ||
            !str_ok(s, c->what_suffix) || !str_ok(s, c->who_prefix) || !str_ok(s, c->who_suffix))
            return fail(err, errcap, "bad character string");
    }
    CHECK_RD("CHRS");

    SECTION("DISP");
    s->ndisps = vn_rd_u32(&r);
    if (s->ndisps > sec.size / 39) return fail(err, errcap, "bad DISP");
    s->disps = vn_calloc(s->ndisps, sizeof *s->disps);
    for (uint32_t i = 0; i < s->ndisps; i++) {
        vn_disp *d = &s->disps[i];
        d->kind = vn_rd_u8(&r);
        d->asset = vn_rd_u16(&r);
        const uint8_t *c = vn_rd_bytes(&r, 4);
        if (c) memcpy(d->color, c, 4);
        d->width = vn_rd_f32(&r);
        d->height = vn_rd_f32(&r);
        d->transform = vn_rd_u16(&r);
        d->child = vn_rd_u16(&r);
        d->text = vn_rd_u32(&r);
        for (int k = 0; k < 4; k++) d->borders[k] = vn_rd_f32(&r);
        if (d->kind > DISP_TRANSFORMED || !str_ok(s, d->text)) return fail(err, errcap, "bad displayable");
    }
    CHECK_RD("DISP");

    SECTION("IMGS");
    s->nimages = vn_rd_u32(&r);
    if (s->nimages > sec.size / 6) return fail(err, errcap, "bad IMGS");
    s->images = vn_calloc(s->nimages, sizeof *s->images);
    for (uint32_t i = 0; i < s->nimages; i++) {
        vn_image *im = &s->images[i];
        im->name = vn_rd_u32(&r);
        im->disp = vn_rd_u16(&r);
        if (r.bad || im->name >= s->nstrings || (im->disp != VN_NONE16 && im->disp >= s->ndisps))
            return fail(err, errcap, "bad image");
        if (!split_image_name(im, vn_str(s, im->name))) return fail(err, errcap, "empty image name");
    }

    SECTION("TRFM");
    s->ntransforms = vn_rd_u32(&r);
    if (s->ntransforms > sec.size / 4) return fail(err, errcap, "bad TRFM");
    s->transforms = vn_calloc(s->ntransforms, sizeof *s->transforms);
    for (uint32_t i = 0; i < s->ntransforms; i++) {
        uint32_t len = vn_rd_u32(&r);
        const uint8_t *p = vn_rd_bytes(&r, len);
        if (!p) return fail(err, errcap, "truncated TRFM");
        s->transforms[i].code = p;
        s->transforms[i].len = len;
    }

    SECTION("TRAN");
    s->ntrans = vn_rd_u32(&r);
    if (s->ntrans > sec.size / 36) return fail(err, errcap, "bad TRAN");
    s->trans = vn_calloc(s->ntrans, sizeof *s->trans);
    for (uint32_t i = 0; i < s->ntrans; i++) {
        vn_trans *t = &s->trans[i];
        t->kind = vn_rd_u8(&r);
        t->warper = vn_rd_u8(&r);
        for (int k = 0; k < 6; k++) t->params[k] = vn_rd_f32(&r);
        const uint8_t *c = vn_rd_bytes(&r, 4);
        if (c) memcpy(t->color, c, 4);
        t->enter = vn_rd_u16(&r);
        t->leave = vn_rd_u16(&r);
        t->enter_warper = vn_rd_u8(&r);
        t->leave_warper = vn_rd_u8(&r);
    }
    CHECK_RD("TRAN");

    SECTION("SHOW");
    s->nshows = vn_rd_u32(&r);
    if (s->nshows > sec.size / 22) return fail(err, errcap, "bad SHOW");
    s->shows = vn_calloc(s->nshows, sizeof *s->shows);
    for (uint32_t i = 0; i < s->nshows; i++) {
        vn_show *sh = &s->shows[i];
        sh->tag = vn_rd_u32(&r);
        sh->nattrs = vn_rd_u8(&r);
        sh->attrs = vn_rd_bytes(&r, 4u * sh->nattrs);
        sh->nat = vn_rd_u8(&r);
        sh->at = vn_rd_bytes(&r, 2u * sh->nat);
        sh->as = vn_rd_u32(&r);
        sh->nbehind = vn_rd_u8(&r);
        sh->behind = vn_rd_bytes(&r, 4u * sh->nbehind);
        sh->zorder = vn_rd_i32(&r);
        sh->has_zorder = vn_rd_u8(&r) != 0;
        vn_rd_u32(&r); /* layer: always master */
        sh->atl = vn_rd_u16(&r);
        sh->disp = vn_rd_u16(&r);
        if (r.bad) return fail(err, errcap, "truncated SHOW");
        bool ok = sh->tag < s->nstrings && str_ok(s, sh->as) &&
                  (sh->atl == VN_NONE16 || sh->atl < s->ntransforms) &&
                  (sh->disp == VN_NONE16 || sh->disp < s->ndisps);
        for (int k = 0; ok && k < sh->nattrs; k++) ok = vn_le32(sh->attrs + 4 * k) < s->nstrings;
        for (int k = 0; ok && k < sh->nbehind; k++) ok = vn_le32(sh->behind + 4 * k) < s->nstrings;
        for (int k = 0; ok && k < sh->nat; k++) ok = vn_le16(sh->at + 2 * k) < s->ntransforms;
        if (!ok) return fail(err, errcap, "bad show record");
    }

    SECTION("CHAN");
    s->nchannels = vn_rd_u32(&r);
    if (s->nchannels > sec.size / 6 || s->nchannels < 4 || s->nchannels > 32) return fail(err, errcap, "bad CHAN");
    s->channels = vn_calloc(s->nchannels, sizeof *s->channels);
    for (uint32_t i = 0; i < s->nchannels; i++) {
        s->channels[i].name = vn_rd_u32(&r);
        s->channels[i].mixer = vn_rd_u8(&r);
        s->channels[i].loop = vn_rd_u8(&r);
        if (s->channels[i].mixer > MIXER_VOICE) return fail(err, errcap, "bad channel mixer");
    }
    CHECK_RD("CHAN");

    SECTION("ASET");
    s->nassets = vn_rd_u32(&r);
    if (s->nassets > sec.size / 5) return fail(err, errcap, "bad ASET");
    s->assets = vn_calloc(s->nassets, sizeof *s->assets);
    for (uint32_t i = 0; i < s->nassets; i++) {
        s->assets[i].key = vn_rd_u32(&r);
        s->assets[i].kind = vn_rd_u8(&r);
        if (s->assets[i].key >= s->nstrings) return fail(err, errcap, "bad asset key");
    }
    CHECK_RD("ASET");

    SECTION("AUDI");
    s->naudio = vn_rd_u32(&r);
    if (s->naudio > sec.size / 15) return fail(err, errcap, "bad AUDI");
    s->audio = vn_calloc(s->naudio, sizeof *s->audio);
    for (uint32_t i = 0; i < s->naudio; i++) {
        vn_audio_rec *a = &s->audio[i];
        a->channel = vn_rd_u8(&r);
        a->flags = vn_rd_u8(&r);
        a->fadein = vn_rd_f32(&r);
        a->fadeout = vn_rd_f32(&r);
        a->volume = vn_rd_f32(&r);
        a->nassets = vn_rd_u8(&r);
        a->assets = vn_rd_bytes(&r, 2u * a->nassets);
        if (r.bad) return fail(err, errcap, "truncated AUDI");
        if (a->channel >= s->nchannels) return fail(err, errcap, "bad audio channel");
        for (int k = 0; k < a->nassets; k++)
            if (vn_le16(a->assets + 2 * k) >= s->nassets) return fail(err, errcap, "bad audio asset");
    }

    for (uint32_t i = 0; i < s->nchars; i++) {
        vn_char *c = &s->chars[i];
        if ((c->side_image != VN_NONE16 && c->side_image >= s->ndisps) || (c->ctc != VN_NONE16 && c->ctc >= s->ndisps))
            return fail(err, errcap, "bad character displayable");
    }
    for (uint32_t i = 0; i < s->ndisps; i++) {
        vn_disp *d = &s->disps[i];
        if ((d->asset != VN_NONE16 && d->asset >= s->nassets) || (d->child != VN_NONE16 && d->child >= s->ndisps) ||
            (d->transform != VN_NONE16 && d->transform >= s->ntransforms))
            return fail(err, errcap, "bad displayable reference");
    }

    if (find_section(data, size, "META", &sec) && sec.size >= 8) {
        uint32_t t = vn_le32(sec.p), v = vn_le32(sec.p + 4);
        if (str_ok(s, t)) s->title = t;
        if (str_ok(s, v)) s->version = v;
    }
#undef SECTION
#undef CHECK_RD
    return true;
}

void vn_script_free(vn_script *s)
{
    for (uint32_t i = 0; i < s->nimages; i++) {
        free(s->images[i].words);
        free((void *)s->images[i].attrs);
    }
    free((void *)s->strings);
    free(s->string_len);
    free(s->sym_name);
    free(s->sym_kind);
    free(s->labels);
    free(s->chars);
    free(s->images);
    free(s->disps);
    free(s->transforms);
    free(s->trans);
    free(s->shows);
    free(s->audio);
    free(s->channels);
    free(s->assets);
    free(s->file);
    memset(s, 0, sizeof *s);
}

uint32_t vn_script_label(const vn_script *s, const char *name, size_t len)
{
    size_t lo = 0, hi = s->nlabels;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        uint32_t id = s->labels[mid].name;
        uint32_t ml = s->string_len[id];
        int c = memcmp(s->strings[id], name, ml < len ? ml : len);
        if (c == 0) c = ml < len ? -1 : ml > len ? 1 : 0;
        if (c == 0) return s->labels[mid].addr;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return VN_NONE32;
}

const char *vn_script_label_at(const vn_script *s, uint32_t addr)
{
    for (uint32_t i = 0; i < s->nlabels; i++)
        if (s->labels[i].addr == addr) return vn_str(s, s->labels[i].name);
    return NULL;
}

const char *vn_op_format(uint8_t op)
{
    return op < OP_COUNT ? op_formats[op] : "";
}

uint32_t vn_insn_length(const vn_script *s, uint32_t pc)
{
    if (pc >= s->code_size) return 0;
    uint8_t op = s->code[pc];
    if (op >= OP_COUNT) return 0;
    uint32_t len = 1;
    for (const char *f = op_formats[op]; *f; f++) {
        switch (*f) {
        case 'b': len += 1; break;
        case 'h': case 'v': len += 2; break;
        case 'i': case 'f': case 's': case 'a': len += 4; break;
        case 'd': len += 8; break;
        case 'S':
            if (pc + len >= s->code_size) return 0;
            len += 1 + 4u * s->code[pc + len];
            break;
        case 'M':
            if (pc + len >= s->code_size) return 0;
            len += 1 + 8u * s->code[pc + len];
            break;
        default: return 0;
        }
    }
    return pc + len <= s->code_size ? len : 0;
}
