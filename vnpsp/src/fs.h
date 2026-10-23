/* File access by path. Game data is read-only; saves go through an atomic
 * temp-file-and-rename write so a crash never leaves a truncated save. */
#ifndef VN_FS_H
#define VN_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct vn_file vn_file;

vn_file *vn_fs_open(const char *path);
bool vn_fs_read_at(vn_file *f, uint64_t offset, void *buf, size_t n);
uint64_t vn_fs_size(vn_file *f);
void vn_fs_close(vn_file *f);

/* Whole file into a malloc'd buffer with a NUL past the end. */
bool vn_fs_read_all(const char *path, void **data, size_t *size);
bool vn_fs_write_atomic(const char *path, const void *data, size_t size);
bool vn_fs_exists(const char *path);
bool vn_fs_remove(const char *path);
bool vn_fs_mkdir(const char *path);   /* true if it exists afterwards */

/* Joins dir and name with exactly one '/' between them. */
void vn_path_join(char *out, size_t cap, const char *dir, const char *name);

#endif
