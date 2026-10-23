/* data.pak reader (docs/dev/pack-format.md) with an LRU cache of decoded image
 * entries, the font atlases, and raw entry access for audio. */
#ifndef VN_ASSETS_H
#define VN_ASSETS_H

#include "fs.h"
#include "gfx.h"
#include "vnb.h"

typedef struct vn_tile {
    uint16_t x, y, w, h;
    gfx_tex *tex;
} vn_tile;

typedef struct vn_texture {
    uint16_t asset;
    uint16_t w, h;            /* PSP pixels */
    uint16_t src_w, src_h;    /* game pixels */
    uint8_t fmt;
    uint8_t ntiles;
    vn_tile *tiles;
    uint8_t *blob;
    uint32_t bytes;
    uint64_t last_used;
    bool bounds_known;
    uint16_t opaque_x0, opaque_y0, opaque_x1, opaque_y1;   /* PSP pixels, exclusive end */
} vn_texture;

typedef struct vn_glyph {
    uint32_t cp;
    uint16_t x, y, w, h;
    int16_t bx, by;
    uint16_t adv64;
    uint8_t page;
} vn_glyph;

typedef struct vn_font {
    uint16_t size;
    int16_t ascent, descent, line_h;
    uint8_t fmt, outline;
    uint8_t npages;
    gfx_tex **pages;
    uint32_t nglyphs;
    vn_glyph *glyphs;
    uint8_t *blob;
} vn_font;

enum { FONT_TEXT, FONT_NAME, FONT_INTERFACE, FONT_ROLES };

typedef struct vn_pack_entry {
    uint32_t name;    /* offset into names */
    uint8_t kind;
    uint32_t offset, size;
} vn_pack_entry;

typedef struct vn_assets {
    vn_file *file;
    uint32_t count;
    vn_pack_entry *entries;
    char *names;
    uint32_t names_len;
    uint32_t nscenes;
    uint32_t *scene_name, *scene_first, *scene_count;
    uint32_t *scene_list;
    uint32_t scene_list_len;

    const vn_script *script;
    int32_t *asset_entry;     /* ASET index -> pack entry, -1 when missing */
    int32_t *label_scene;     /* LABL index -> scene, -1 when none */

    vn_texture **textures;    /* by ASET index, NULL when not loaded */
    size_t budget, used;
    uint64_t frame;

    vn_font *fonts[FONT_ROLES];
    uint32_t nfaces;
    vn_font **faces;          /* font/face/<file> entries */
    char **face_names;

    uint64_t bytes_read;      /* statistics for load-time reports */
    uint32_t loads;
} vn_assets;

bool vn_assets_open(vn_assets *a, const char *path, const vn_script *s, size_t budget, char *err, size_t errcap);
void vn_assets_close(vn_assets *a);

/* Marks the start of a frame: textures used from now on are protected from eviction. */
void vn_assets_new_frame(vn_assets *a);
vn_texture *vn_assets_texture(vn_assets *a, uint16_t asset);
/* Loads every image of the scene that starts at this label address; returns bytes read. */
uint64_t vn_assets_prefetch_label(vn_assets *a, uint32_t addr);
/* Raw entry bytes for an ASET index (malloc'd; caller frees). */
bool vn_assets_read(vn_assets *a, uint16_t asset, uint8_t **data, uint32_t *size);

const vn_font *vn_assets_font(const vn_assets *a, int role);
const vn_font *vn_assets_face(const vn_assets *a, const char *name, size_t len);
const vn_glyph *vn_font_glyph(const vn_font *f, uint32_t cp);

/* Bounding box of non-transparent pixels (block precision). */
void vn_texture_bounds(vn_texture *t);

void *vn_aligned_alloc(size_t size);
void vn_aligned_free(void *p);

#endif
