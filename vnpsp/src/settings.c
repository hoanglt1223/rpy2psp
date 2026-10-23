/* Player preferences in <save dir>/settings.bin: text speed, auto-forward delay, volumes. */
#include "engine_internal.h"
#include "fs.h"

#include <stdlib.h>
#include <string.h>

#define SETTINGS_FILE "settings.bin"

void settings_default(settings *s)
{
    s->cps = 40.0f;
    s->auto_delay = 2.0f;
    s->vol_music = 0.8f;
    s->vol_sfx = 0.8f;
    s->vol_voice = 1.0f;
}

void settings_apply(vn_engine *e)
{
    if (e->mixer) mixer_volumes(e->mixer, e->set.vol_music, e->set.vol_sfx, e->set.vol_voice);
}

void settings_load(vn_engine *e)
{
    settings_default(&e->set);
    char path[512];
    vn_path_join(path, sizeof path, e->save_dir, SETTINGS_FILE);
    void *data;
    size_t size;
    if (!vn_fs_read_all(path, &data, &size)) return;
    vn_rd r;
    vn_rd_init(&r, data, size);
    const uint8_t *magic = vn_rd_bytes(&r, 4);
    uint32_t version = vn_rd_u32(&r);
    settings s;
    s.cps = vn_rd_f32(&r);
    s.auto_delay = vn_rd_f32(&r);
    s.vol_music = vn_rd_f32(&r);
    s.vol_sfx = vn_rd_f32(&r);
    s.vol_voice = vn_rd_f32(&r);
    uint32_t crc = vn_rd_u32(&r);
    bool ok = !r.bad && magic && memcmp(magic, "VNST", 4) == 0 && version == 1 &&
              crc == vn_crc32(0, data, size - 4) && s.cps >= 0 && s.cps <= 1000 && s.auto_delay >= 0.1f &&
              s.auto_delay <= 30 && s.vol_music >= 0 && s.vol_music <= 1 && s.vol_sfx >= 0 && s.vol_sfx <= 1 &&
              s.vol_voice >= 0 && s.vol_voice <= 1;
    free(data);
    if (ok)
        e->set = s;
    else
        vn_log("settings: %s is invalid; using defaults", path);
}

static void put_f32(vn_buf *b, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    vn_buf_u32(b, u);
}

void settings_save(vn_engine *e)
{
    vn_buf b = {0};
    vn_buf_put(&b, "VNST", 4);
    vn_buf_u32(&b, 1);
    put_f32(&b, e->set.cps);
    put_f32(&b, e->set.auto_delay);
    put_f32(&b, e->set.vol_music);
    put_f32(&b, e->set.vol_sfx);
    put_f32(&b, e->set.vol_voice);
    vn_buf_u32(&b, vn_crc32(0, b.data, b.len));
    char path[512];
    vn_path_join(path, sizeof path, e->save_dir, SETTINGS_FILE);
    if (!vn_fs_write_atomic(path, b.data, b.len)) eng_message(e, "Could not save the settings.");
    vn_buf_free(&b);
}
