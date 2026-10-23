#include "plat_internal.h"

#include <SDL3/SDL.h>

#include <stdlib.h>

/* Owner directory shared by every app using this layer on PC. */
#define PREF_ORG "anyport"

struct plat_file {
    SDL_IOStream *io;
};

static const char *data_dir;
static char *user_dir;

plat_result plat__files_init(const char *app_id)
{
    if (!plat__app_id_is_valid(app_id)) return PLAT_ERR_INVALID;
    plat__files_shutdown();
    data_dir = SDL_GetBasePath();
    if (!data_dir) {
        plat_log("plat: SDL_GetBasePath failed: %s", SDL_GetError());
        return PLAT_ERR_BACKEND;
    }
    user_dir = SDL_GetPrefPath(PREF_ORG, app_id);
    if (!user_dir) {
        plat_log("plat: SDL_GetPrefPath failed: %s", SDL_GetError());
        data_dir = NULL;
        return PLAT_ERR_BACKEND;
    }
    return PLAT_OK;
}

void plat__files_shutdown(void)
{
    SDL_free(user_dir);
    user_dir = NULL;
    data_dir = NULL;
}

const char *plat__root_dir(plat_root root)
{
    return root == PLAT_ROOT_USER ? user_dir : data_dir;
}

static bool path_exists(const char *full)
{
    return SDL_GetPathInfo(full, NULL);
}

plat_result plat_file_open(plat_root root, const char *rel, plat_open_mode mode, plat_file **out)
{
    if (!out) return PLAT_ERR_INVALID;
    *out = NULL;
    if (mode != PLAT_OPEN_READ && mode != PLAT_OPEN_WRITE) return PLAT_ERR_INVALID;
    if (mode == PLAT_OPEN_WRITE && root != PLAT_ROOT_USER) return PLAT_ERR_INVALID;

    char full[PLAT_PATH_MAX * 2];
    plat_result r = plat__full_path(root, rel, full, sizeof full);
    if (r != PLAT_OK) return r;

    plat_file *f = malloc(sizeof *f);
    if (!f) return PLAT_ERR_NO_MEMORY;
    f->io = SDL_IOFromFile(full, mode == PLAT_OPEN_READ ? "rb" : "wb");
    if (!f->io) {
        free(f);
        return (mode == PLAT_OPEN_READ && !path_exists(full)) ? PLAT_ERR_NOT_FOUND : PLAT_ERR_IO;
    }
    *out = f;
    return PLAT_OK;
}

int64_t plat_file_read(plat_file *f, void *buf, size_t size)
{
    if (!f || (!buf && size)) return PLAT_ERR_INVALID;
    size_t total = 0;
    while (total < size) {
        size_t got = SDL_ReadIO(f->io, (char *)buf + total, size - total);
        if (got == 0) {
            if (SDL_GetIOStatus(f->io) == SDL_IO_STATUS_EOF) break;
            return PLAT_ERR_IO;
        }
        total += got;
    }
    return (int64_t)total;
}

int64_t plat_file_write(plat_file *f, const void *buf, size_t size)
{
    if (!f || (!buf && size)) return PLAT_ERR_INVALID;
    size_t put = SDL_WriteIO(f->io, buf, size);
    return put == size ? (int64_t)put : PLAT_ERR_IO;
}

plat_result plat_file_seek(plat_file *f, int64_t offset)
{
    if (!f || offset < 0) return PLAT_ERR_INVALID;
    return SDL_SeekIO(f->io, offset, SDL_IO_SEEK_SET) < 0 ? PLAT_ERR_IO : PLAT_OK;
}

int64_t plat_file_size(plat_file *f)
{
    if (!f) return PLAT_ERR_INVALID;
    Sint64 n = SDL_GetIOSize(f->io);
    return n < 0 ? PLAT_ERR_IO : n;
}

plat_result plat_file_close(plat_file *f)
{
    if (!f) return PLAT_ERR_INVALID;
    bool ok = SDL_CloseIO(f->io);
    free(f);
    return ok ? PLAT_OK : PLAT_ERR_IO;
}

plat_result plat__replace_file(const char *from_full, const char *to_full)
{
    /* SDL_RenamePath replaces an existing target in one step on Windows and POSIX. */
    if (SDL_RenamePath(from_full, to_full)) return PLAT_OK;
    plat_log("plat: rename '%s' -> '%s' failed: %s", from_full, to_full, SDL_GetError());
    return PLAT_ERR_IO;
}

plat_result plat__remove_full(const char *full)
{
    /* SDL_RemovePath reports success for a missing path; callers need to tell the two apart. */
    if (!path_exists(full)) return PLAT_ERR_NOT_FOUND;
    if (SDL_RemovePath(full)) return PLAT_OK;
    plat_log("plat: remove '%s' failed: %s", full, SDL_GetError());
    return PLAT_ERR_IO;
}

plat_result plat_mkdir(plat_root root, const char *rel)
{
    if (root != PLAT_ROOT_USER) return PLAT_ERR_INVALID;
    char full[PLAT_PATH_MAX * 2];
    plat_result r = plat__full_path(root, rel, full, sizeof full);
    if (r != PLAT_OK) return r;
    if (SDL_CreateDirectory(full)) return PLAT_OK;
    SDL_PathInfo info;
    if (SDL_GetPathInfo(full, &info) && info.type == SDL_PATHTYPE_DIRECTORY) return PLAT_OK;
    plat_log("plat: mkdir '%s' failed: %s", full, SDL_GetError());
    return PLAT_ERR_IO;
}
