#ifndef PNG_WRITE_H
#define PNG_WRITE_H

#include <stdbool.h>
#include <stdint.h>

/* Writes 8-bit RGB pixels (top row first) as an uncompressed PNG. */
bool png_write_rgb(const char *path, const uint8_t *rgb, int width, int height);

#endif
