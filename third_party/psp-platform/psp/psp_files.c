#include "plat_internal.h"

#include <pspiofilemgr.h>
#include <pspkernel.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ERR_ENOENT 0x80010002
#define ERR_EEXIST 0x80010011

struct plat_file {
    SceUID fd;
};

static char data_dir[PLAT_PATH_MAX];
static char user_dir[PLAT_PATH_MAX];
static bool files_ready;

static plat_result mkdir_full(const char *full)
{
    int r = sceIoMkdir(full, 0777);
    if (r >= 0 || r == (int)ERR_EEXIST) return PLAT_OK;
    plat_log("plat: sceIoMkdir '%s' failed 0x%08x", full, (unsigned)r);
    return PLAT_ERR_IO;
}

plat_result plat__files_init(const char *app_id)
{
    if (!plat__app_id_is_valid(app_id)) return PLAT_ERR_INVALID;
    files_ready = false;

    /* libcglue sets the cwd to the EBOOT folder (or host0: under psplink). */
    if (!getcwd(data_dir, sizeof data_dir - 1) || data_dir[0] == '\0') {
        plat_log("plat: getcwd failed");
        return PLAT_ERR_BACKEND;
    }
    size_t n = strlen(data_dir);
    if (data_dir[n - 1] != '/') {
        data_dir[n] = '/';
        data_dir[n + 1] = '\0';
    }

    plat_result r = mkdir_full("ms0:/PSP/SAVEDATA");
    if (r != PLAT_OK) return r;
    snprintf(user_dir, sizeof user_dir, "ms0:/PSP/SAVEDATA/%s", app_id);
    r = mkdir_full(user_dir);
    if (r != PLAT_OK) return r;
    strcat(user_dir, "/");
    files_ready = true;
    return PLAT_OK;
}

void plat__files_shutdown(void)
{
    files_ready = false;
}

const char *plat__root_dir(plat_root root)
{
    if (!files_ready) return NULL;
    return root == PLAT_ROOT_USER ? user_dir : data_dir;
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

    int flags = mode == PLAT_OPEN_READ ? PSP_O_RDONLY : (PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC);
    SceUID fd = sceIoOpen(full, flags, 0777);
    if (fd < 0) return fd == (SceUID)ERR_ENOENT ? PLAT_ERR_NOT_FOUND : PLAT_ERR_IO;

    plat_file *f = malloc(sizeof *f);
    if (!f) {
        sceIoClose(fd);
        return PLAT_ERR_NO_MEMORY;
    }
    f->fd = fd;
    *out = f;
    return PLAT_OK;
}

int64_t plat_file_read(plat_file *f, void *buf, size_t size)
{
    if (!f || (!buf && size)) return PLAT_ERR_INVALID;
    size_t total = 0;
    while (total < size) {
        size_t chunk = size - total > 0x40000000u ? 0x40000000u : size - total;
        int got = sceIoRead(f->fd, (char *)buf + total, (SceSize)chunk);
        if (got < 0) return PLAT_ERR_IO;
        if (got == 0) break;
        total += (size_t)got;
    }
    return (int64_t)total;
}

int64_t plat_file_write(plat_file *f, const void *buf, size_t size)
{
    if (!f || (!buf && size)) return PLAT_ERR_INVALID;
    size_t total = 0;
    while (total < size) {
        size_t chunk = size - total > 0x40000000u ? 0x40000000u : size - total;
        int put = sceIoWrite(f->fd, (const char *)buf + total, (SceSize)chunk);
        if (put <= 0) return PLAT_ERR_IO;
        total += (size_t)put;
    }
    return (int64_t)total;
}

plat_result plat_file_seek(plat_file *f, int64_t offset)
{
    if (!f || offset < 0) return PLAT_ERR_INVALID;
    return sceIoLseek(f->fd, offset, PSP_SEEK_SET) == offset ? PLAT_OK : PLAT_ERR_IO;
}

int64_t plat_file_size(plat_file *f)
{
    if (!f) return PLAT_ERR_INVALID;
    SceOff cur = sceIoLseek(f->fd, 0, PSP_SEEK_CUR);
    SceOff end = sceIoLseek(f->fd, 0, PSP_SEEK_END);
    if (cur < 0 || end < 0 || sceIoLseek(f->fd, cur, PSP_SEEK_SET) != cur) return PLAT_ERR_IO;
    return end;
}

plat_result plat_file_close(plat_file *f)
{
    if (!f) return PLAT_ERR_INVALID;
    int r = sceIoClose(f->fd);
    free(f);
    return r < 0 ? PLAT_ERR_IO : PLAT_OK;
}

plat_result plat__replace_file(const char *from_full, const char *to_full)
{
    if (sceIoRename(from_full, to_full) >= 0) return PLAT_OK;
    /* FAT rename refuses an existing target. The old file is removed only after
     * the new one is fully written, so a crash here leaves the .tmp copy intact. */
    int rr = sceIoRemove(to_full);
    if (rr < 0 && rr != (int)ERR_ENOENT) {
        plat_log("plat: sceIoRemove '%s' failed 0x%08x", to_full, (unsigned)rr);
        return PLAT_ERR_IO;
    }
    int r = sceIoRename(from_full, to_full);
    if (r < 0) {
        plat_log("plat: sceIoRename '%s' failed 0x%08x", from_full, (unsigned)r);
        return PLAT_ERR_IO;
    }
    return PLAT_OK;
}

plat_result plat__remove_full(const char *full)
{
    int r = sceIoRemove(full);
    if (r >= 0) return PLAT_OK;
    return r == (int)ERR_ENOENT ? PLAT_ERR_NOT_FOUND : PLAT_ERR_IO;
}

plat_result plat_mkdir(plat_root root, const char *rel)
{
    if (root != PLAT_ROOT_USER) return PLAT_ERR_INVALID;
    char full[PLAT_PATH_MAX * 2];
    plat_result r = plat__full_path(root, rel, full, sizeof full);
    if (r != PLAT_OK) return r;
    return mkdir_full(full);
}
