#include "plat_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

bool plat__app_id_is_valid(const char *app_id)
{
    if (!app_id) return false;
    size_t n = strlen(app_id);
    if (n == 0 || n > PLAT_APP_ID_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = app_id[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool plat_path_is_valid(const char *rel)
{
    if (!rel) return false;
    size_t n = strlen(rel);
    if (n == 0 || n >= PLAT_PATH_MAX) return false;

    size_t seg_start = 0;
    for (size_t i = 0; i <= n; i++) {
        char c = rel[i];
        if (c == '\\' || c == ':' || (c > 0 && c < 0x20)) return false;
        if (c == '/' || c == '\0') {
            size_t len = i - seg_start;
            if (len == 0) return false;
            if (len == 1 && rel[seg_start] == '.') return false;
            if (len == 2 && rel[seg_start] == '.' && rel[seg_start + 1] == '.') return false;
            seg_start = i + 1;
        }
    }
    return true;
}

void plat__apply_deadzone(float *x, float *y)
{
    float mag = sqrtf(*x * *x + *y * *y);
    if (mag < PLAT_STICK_DEADZONE) {
        *x = 0.0f;
        *y = 0.0f;
        return;
    }
    float clamped = mag > 1.0f ? 1.0f : mag;
    float scale = (clamped - PLAT_STICK_DEADZONE) / (1.0f - PLAT_STICK_DEADZONE) / mag;
    *x *= scale;
    *y *= scale;
}

void plat__input_update(plat_input *state, uint32_t held, float stick_x, float stick_y)
{
    uint32_t prev = state->held;
    plat__apply_deadzone(&stick_x, &stick_y);
    state->held = held;
    state->pressed = held & ~prev;
    state->released = prev & ~held;
    state->stick_x = stick_x;
    state->stick_y = stick_y;
}

plat_result plat__full_path(plat_root root, const char *rel, char *buf, size_t buf_size)
{
    if (!plat_path_is_valid(rel) || !buf) return PLAT_ERR_INVALID;
    if (root != PLAT_ROOT_DATA && root != PLAT_ROOT_USER) return PLAT_ERR_INVALID;
    const char *dir = plat__root_dir(root);
    if (!dir) return PLAT_ERR_STATE;
    size_t dl = strlen(dir), rl = strlen(rel);
    if (dl + rl + 1 > buf_size) return PLAT_ERR_INVALID;
    memcpy(buf, dir, dl);
    memcpy(buf + dl, rel, rl + 1);
    return PLAT_OK;
}

plat_result plat_file_read_all(plat_root root, const char *rel, void **data, size_t *size)
{
    if (!data || !size) return PLAT_ERR_INVALID;
    *data = NULL;
    *size = 0;

    plat_file *f;
    plat_result r = plat_file_open(root, rel, PLAT_OPEN_READ, &f);
    if (r != PLAT_OK) return r;

    int64_t len = plat_file_size(f);
    if (len < 0 || (uint64_t)len >= (uint64_t)SIZE_MAX) {
        plat_file_close(f);
        return len < 0 ? (plat_result)len : PLAT_ERR_NO_MEMORY;
    }
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        plat_file_close(f);
        return PLAT_ERR_NO_MEMORY;
    }
    int64_t got = plat_file_read(f, buf, (size_t)len);
    plat_result cr = plat_file_close(f);
    if (got != len || cr != PLAT_OK) {
        free(buf);
        return got < 0 ? (plat_result)got : PLAT_ERR_IO;
    }
    buf[len] = '\0';
    *data = buf;
    *size = (size_t)len;
    return PLAT_OK;
}

plat_result plat_file_write_all(plat_root root, const char *rel, const void *data, size_t size)
{
    if (root != PLAT_ROOT_USER) return PLAT_ERR_INVALID;
    if (!data && size > 0) return PLAT_ERR_INVALID;
    if (!plat_path_is_valid(rel)) return PLAT_ERR_INVALID;

    char tmp_rel[PLAT_PATH_MAX];
    size_t rl = strlen(rel);
    if (rl + 4 >= sizeof tmp_rel) return PLAT_ERR_INVALID;
    memcpy(tmp_rel, rel, rl);
    memcpy(tmp_rel + rl, ".tmp", 5);

    char tmp_full[PLAT_PATH_MAX * 2], final_full[PLAT_PATH_MAX * 2];
    plat_result r = plat__full_path(root, tmp_rel, tmp_full, sizeof tmp_full);
    if (r == PLAT_OK) r = plat__full_path(root, rel, final_full, sizeof final_full);
    if (r != PLAT_OK) return r;

    plat_file *f;
    r = plat_file_open(root, tmp_rel, PLAT_OPEN_WRITE, &f);
    if (r != PLAT_OK) return r;
    int64_t put = size ? plat_file_write(f, data, size) : 0;
    plat_result cr = plat_file_close(f);
    if (put != (int64_t)size || cr != PLAT_OK) {
        plat__remove_full(tmp_full);
        return put < 0 ? (plat_result)put : (cr != PLAT_OK ? cr : PLAT_ERR_IO);
    }

    r = plat__replace_file(tmp_full, final_full);
    if (r != PLAT_OK) plat__remove_full(tmp_full);
    return r;
}

plat_result plat_file_remove(plat_root root, const char *rel)
{
    if (root != PLAT_ROOT_USER) return PLAT_ERR_INVALID;
    char full[PLAT_PATH_MAX * 2];
    plat_result r = plat__full_path(root, rel, full, sizeof full);
    if (r != PLAT_OK) return r;
    return plat__remove_full(full);
}
