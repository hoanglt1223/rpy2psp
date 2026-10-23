/* script.vnb loader (format: docs/dev/bytecode.md). Every table is bounds-checked
 * at load time; table indexes found later in code are checked at use. */
#ifndef VN_VNB_H
#define VN_VNB_H

#include "base.h"

enum {
    OP_NOP, OP_PUSH_NONE, OP_PUSH_TRUE, OP_PUSH_FALSE, OP_PUSH_INT, OP_PUSH_FLOAT, OP_PUSH_STR,
    OP_LOAD, OP_STORE, OP_STORE_DEFAULT, OP_POP, OP_DUP,
    OP_BUILD_LIST, OP_BUILD_TUPLE, OP_BUILD_DICT, OP_BUILD_SET,
    OP_SUBSCR, OP_SLICE, OP_STORE_SUBSCR, OP_BINARY, OP_UNARY, OP_COMPARE,
    OP_JUMP, OP_JUMP_IF_FALSE, OP_JUMP_IF_TRUE, OP_JUMP_IF_FALSE_OR_POP, OP_JUMP_IF_TRUE_OR_POP,
    OP_CALL_FN, OP_CALL_METHOD, OP_CALL, OP_CALL_DYNAMIC, OP_JUMP_DYNAMIC, OP_RETURN, OP_BIND,
    OP_SAY, OP_SAY_DYNAMIC, OP_SAY_ATTRS, OP_MENU, OP_PAUSE, OP_WITH,
    OP_SHOW, OP_SHOW_DYNAMIC, OP_SCENE, OP_HIDE, OP_PLAY, OP_STOP, OP_VOICE, OP_VOICE_SUSTAIN,
    OP_WINDOW, OP_NVL, OP_MOVIE,
    OP_COUNT
};

enum { CHAR_CENTERED = 1, CHAR_EXTEND = 2, CHAR_DYNAMIC_NAME = 4 };
enum { CHAR_NARRATOR = 0, CHAR_CENTERED_BUILTIN = 1, CHAR_NVL_NARRATOR = 2, CHAR_EXTEND_BUILTIN = 3 };
enum { DISP_TEXTURE, DISP_SOLID, DISP_NULL, DISP_FRAME, DISP_ATL, DISP_TEXT, DISP_TRANSFORMED };
enum { TR_DISSOLVE = 1, TR_FADE, TR_PIXELLATE, TR_MOVE, TR_CROPMOVE, TR_PUSHMOVE, TR_SHAKE, TR_PAUSE };
enum { ASSET_IMAGE, ASSET_MUSIC, ASSET_SFX, ASSET_VOICE, ASSET_FONT, ASSET_MOVIE };
enum { AUDIO_LOOP = 1, AUDIO_IF_CHANGED = 4, AUDIO_QUEUE = 8 };
enum { MIXER_MUSIC, MIXER_SFX, MIXER_VOICE };

typedef struct vn_char {
    uint32_t name;
    uint8_t who_color[4], what_color[4];
    uint8_t kind, flags;
    uint32_t image;
    uint16_t side_image;
    uint32_t what_prefix, what_suffix, who_prefix, who_suffix;
    uint16_t ctc;
} vn_char;

typedef struct vn_image {
    uint32_t name;
    uint16_t disp;
    /* The name split at spaces: tag, then attributes (pointers into `words`). */
    char *words;
    const char *tag;
    uint8_t nattrs;
    const char **attrs;
} vn_image;

typedef struct vn_disp {
    uint8_t kind;
    uint16_t asset;
    uint8_t color[4];
    float width, height;
    uint16_t transform, child;
    uint32_t text;
    float borders[4];
} vn_disp;

typedef struct vn_atl {
    const uint8_t *code;
    uint32_t len;
} vn_atl;

typedef struct vn_trans {
    uint8_t kind, warper;
    float params[6];
    uint8_t color[4];
    uint16_t enter, leave;
    uint8_t enter_warper, leave_warper;
} vn_trans;

typedef struct vn_show {
    uint32_t tag;
    uint8_t nattrs;
    const uint8_t *attrs;    /* nattrs u32 string ids */
    uint8_t nat;
    const uint8_t *at;       /* nat u16 transform ids */
    uint32_t as;
    uint8_t nbehind;
    const uint8_t *behind;   /* nbehind u32 string ids */
    int32_t zorder;
    bool has_zorder;
    uint16_t atl, disp;
} vn_show;

typedef struct vn_audio_rec {
    uint8_t channel, flags;
    float fadein, fadeout, volume;
    uint8_t nassets;
    const uint8_t *assets;   /* u16 asset ids */
} vn_audio_rec;

typedef struct vn_channel_def {
    uint32_t name;
    uint8_t mixer, loop;
} vn_channel_def;

typedef struct vn_asset_def {
    uint32_t key;
    uint8_t kind;
} vn_asset_def;

typedef struct vn_label {
    uint32_t name, addr;
} vn_label;

typedef struct vn_script {
    uint8_t *file;
    size_t size;
    uint64_t hash;

    uint32_t init_addr, defaults_addr, start_addr;
    float src_w, src_h;
    const uint8_t *code;
    uint32_t code_size;

    uint32_t nstrings;
    const char **strings;
    uint32_t *string_len;

    uint32_t nsyms;
    uint32_t *sym_name;
    uint8_t *sym_kind;          /* 0 store, 1 persistent */

    uint32_t nlabels;
    vn_label *labels;           /* sorted by name, as stored */

    uint32_t nchars;
    vn_char *chars;
    uint32_t nimages;
    vn_image *images;
    uint32_t ndisps;
    vn_disp *disps;
    uint32_t ntransforms;
    vn_atl *transforms;
    uint32_t ntrans;
    vn_trans *trans;
    uint32_t nshows;
    vn_show *shows;
    uint32_t naudio;
    vn_audio_rec *audio;
    uint32_t nchannels;
    vn_channel_def *channels;
    uint32_t nassets;
    vn_asset_def *assets;

    uint32_t title, version;    /* META; VN_NONE32 when absent */
} vn_script;

/* Takes ownership of `data` (malloc'd). On failure writes a reason into err. */
bool vn_script_load(vn_script *s, uint8_t *data, size_t size, char *err, size_t errcap);
void vn_script_free(vn_script *s);

static inline const char *vn_str(const vn_script *s, uint32_t id)
{
    return id < s->nstrings ? s->strings[id] : "";
}
/* Label address by name, or VN_NONE32. */
uint32_t vn_script_label(const vn_script *s, const char *name, size_t len);
/* Label name whose address is addr (first match), or NULL. */
const char *vn_script_label_at(const vn_script *s, uint32_t addr);
/* Operand codes of an opcode (bytecode.md), "" when unknown. */
const char *vn_op_format(uint8_t op);
/* Byte length of the instruction at pc, or 0 if it is malformed. */
uint32_t vn_insn_length(const vn_script *s, uint32_t pc);

#endif
