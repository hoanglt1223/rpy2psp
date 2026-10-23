#include "fs.h"

#include "base.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

struct vn_file {
    FILE *f;
    uint64_t size;
};

static bool seek_to(FILE *f, uint64_t off)
{
#ifdef _WIN32
    return _fseeki64(f, (long long)off, SEEK_SET) == 0;
#else
    return fseek(f, (long)off, SEEK_SET) == 0;
#endif
}

vn_file *vn_fs_open(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    long long size = -1;
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END) == 0) size = _ftelli64(f);
#else
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
#endif
    if (size < 0 || !seek_to(f, 0)) {
        fclose(f);
        return NULL;
    }
    vn_file *vf = vn_alloc(sizeof *vf);
    vf->f = f;
    vf->size = (uint64_t)size;
    return vf;
}

bool vn_fs_read_at(vn_file *f, uint64_t offset, void *buf, size_t n)
{
    if (!f || offset > f->size || n > f->size - offset) return false;
    return seek_to(f->f, offset) && fread(buf, 1, n, f->f) == n;
}

uint64_t vn_fs_size(vn_file *f)
{
    return f ? f->size : 0;
}

void vn_fs_close(vn_file *f)
{
    if (!f) return;
    fclose(f->f);
    free(f);
}

bool vn_fs_read_all(const char *path, void **data, size_t *size)
{
    *data = NULL;
    *size = 0;
    vn_file *f = vn_fs_open(path);
    if (!f) return false;
    if (f->size >= (uint64_t)SIZE_MAX) {
        vn_fs_close(f);
        return false;
    }
    size_t n = (size_t)f->size;
    char *buf = vn_alloc(n + 1);
    bool ok = vn_fs_read_at(f, 0, buf, n);
    vn_fs_close(f);
    if (!ok) {
        free(buf);
        return false;
    }
    buf[n] = '\0';
    *data = buf;
    *size = n;
    return true;
}

static bool replace_file(const char *from, const char *to)
{
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (rename(from, to) == 0) return true;
    /* FAT (Memory Stick) refuses to rename over an existing file. The old file
     * goes only after the new one is complete, so a crash keeps the .tmp copy. */
    remove(to);
    return rename(from, to) == 0;
#endif
}

bool vn_fs_write_atomic(const char *path, const void *data, size_t size)
{
    char tmp[512];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof tmp) return false;
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        vn_log("fs: cannot create %s", tmp);
        return false;
    }
    bool ok = size == 0 || fwrite(data, 1, size, f) == size;
    if (fclose(f) != 0) ok = false;
    if (ok) ok = replace_file(tmp, path);
    if (!ok) {
        vn_log("fs: writing %s failed", path);
        remove(tmp);
    }
    return ok;
}

bool vn_fs_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

bool vn_fs_remove(const char *path)
{
    return remove(path) == 0;
}

static bool mkdir_one(const char *path)
{
#ifdef _WIN32
    if (_mkdir(path) == 0) return true;
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    if (mkdir(path, 0777) == 0) return true;
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool vn_fs_mkdir(const char *path)
{
    char buf[512];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof buf) return false;
    memcpy(buf, path, n + 1);
    while (n > 1 && (buf[n - 1] == '/' || buf[n - 1] == '\\')) buf[--n] = 0;
    /* Create each missing parent; skip the root and drive or device prefixes such as "C:" or "ms0:". */
    for (size_t i = 1; i < n; i++) {
        if ((buf[i] == '/' || buf[i] == '\\') && buf[i - 1] != ':') {
            char c = buf[i];
            buf[i] = 0;
            mkdir_one(buf);
            buf[i] = c;
        }
    }
    return mkdir_one(buf);
}

void vn_path_join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t dl = strlen(dir);
    bool slash = dl > 0 && (dir[dl - 1] == '/' || dir[dl - 1] == '\\');
    snprintf(out, cap, "%s%s%s", dir, (slash || dl == 0) ? "" : "/", name);
}
