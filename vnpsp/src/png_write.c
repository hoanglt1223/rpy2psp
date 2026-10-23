/* Minimal PNG writer: stored (uncompressed) deflate blocks, so no zlib is needed.
 * From AnySWF apps/player/png_write.c (same author, MIT). */
#include "png_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    c ^= 0xFFFFFFFFu;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    uint32_t c = crc(crc(0, hdr + 4, 4), data, len);
    uint8_t tail[4];
    be32(tail, c);
    return fwrite(hdr, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) && fwrite(tail, 1, 4, f) == 4;
}

bool png_write_rgb(const char *path, const uint8_t *rgb, int w, int h)
{
    crc_init();
    size_t row = (size_t)w * 3 + 1, raw_len = row * h;
    uint8_t *raw = malloc(raw_len);
    if (!raw) return false;
    for (int y = 0; y < h; y++) {
        raw[y * row] = 0;
        memcpy(raw + y * row + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    size_t blocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + blocks * 5 + 4;
    uint8_t *z = malloc(z_len), *o = z;
    if (!z) {
        free(raw);
        return false;
    }
    *o++ = 0x78;
    *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t off = 0; off < raw_len; off += 65535) {
        size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
        *o++ = off + n == raw_len ? 1 : 0;
        *o++ = (uint8_t)n;
        *o++ = (uint8_t)(n >> 8);
        *o++ = (uint8_t)~n;
        *o++ = (uint8_t)(~n >> 8);
        memcpy(o, raw + off, n);
        o += n;
    }
    be32(o, (b << 16) | a);
    free(raw);

    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* RGB */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite("\x89PNG\r\n\x1a\n", 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) &&
              chunk(f, "IDAT", z, (uint32_t)z_len) && chunk(f, "IEND", NULL, 0);
    if (f && fclose(f) != 0) ok = false;
    free(z);
    return ok;
}
