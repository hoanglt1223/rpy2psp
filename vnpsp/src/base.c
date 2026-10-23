#include "base.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void (*log_sink)(const char *line);

void vn_set_log_sink(void (*sink)(const char *line))
{
    log_sink = sink;
}

void vn_log(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (log_sink) {
        log_sink(line);
    } else {
        fputs(line, stderr);
        fputc('\n', stderr);
    }
}

static void out_of_memory(size_t size)
{
    vn_log("vnpsp: out of memory (%zu bytes)", size);
    abort();
}

void *vn_alloc(size_t size)
{
    void *p = malloc(size ? size : 1);
    if (!p) out_of_memory(size);
    return p;
}

void *vn_calloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p) out_of_memory(n * size);
    return p;
}

void *vn_realloc(void *p, size_t size)
{
    void *q = realloc(p, size ? size : 1);
    if (!q) out_of_memory(size);
    return q;
}

char *vn_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = vn_alloc(n);
    memcpy(d, s, n);
    return d;
}

void vn_buf_reserve(vn_buf *b, size_t extra)
{
    if (b->len + extra <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra) cap *= 2;
    b->data = vn_realloc(b->data, cap);
    b->cap = cap;
}

void vn_buf_put(vn_buf *b, const void *data, size_t n)
{
    if (!n) return;
    vn_buf_reserve(b, n);
    memcpy(b->data + b->len, data, n);
    b->len += n;
}

void vn_buf_u8(vn_buf *b, uint8_t v)
{
    vn_buf_put(b, &v, 1);
}

void vn_buf_u16(vn_buf *b, uint16_t v)
{
    uint8_t t[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    vn_buf_put(b, t, 2);
}

void vn_buf_u32(vn_buf *b, uint32_t v)
{
    uint8_t t[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    vn_buf_put(b, t, 4);
}

void vn_buf_u64(vn_buf *b, uint64_t v)
{
    vn_buf_u32(b, (uint32_t)v);
    vn_buf_u32(b, (uint32_t)(v >> 32));
}

void vn_buf_f64(vn_buf *b, double v)
{
    uint64_t u;
    memcpy(&u, &v, 8);
    vn_buf_u64(b, u);
}

void vn_buf_str(vn_buf *b, const char *s, size_t n)
{
    vn_buf_u32(b, (uint32_t)n);
    vn_buf_put(b, s, n);
}

void vn_buf_free(vn_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

void vn_rd_init(vn_rd *r, const void *data, size_t len)
{
    r->p = data;
    r->len = data ? len : 0;
    r->pos = 0;
    r->bad = false;
}

const uint8_t *vn_rd_bytes(vn_rd *r, size_t n)
{
    if (r->bad || n > r->len - r->pos) {
        r->bad = true;
        return NULL;
    }
    const uint8_t *p = r->p + r->pos;
    r->pos += n;
    return p;
}

uint8_t vn_rd_u8(vn_rd *r)
{
    const uint8_t *p = vn_rd_bytes(r, 1);
    return p ? p[0] : 0;
}

uint16_t vn_rd_u16(vn_rd *r)
{
    const uint8_t *p = vn_rd_bytes(r, 2);
    return p ? vn_le16(p) : 0;
}

uint32_t vn_rd_u32(vn_rd *r)
{
    const uint8_t *p = vn_rd_bytes(r, 4);
    return p ? vn_le32(p) : 0;
}

int32_t vn_rd_i32(vn_rd *r)
{
    return (int32_t)vn_rd_u32(r);
}

uint64_t vn_rd_u64(vn_rd *r)
{
    uint64_t lo = vn_rd_u32(r);
    return lo | ((uint64_t)vn_rd_u32(r) << 32);
}

float vn_lef32(const uint8_t *p)
{
    uint32_t u = vn_le32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

float vn_rd_f32(vn_rd *r)
{
    const uint8_t *p = vn_rd_bytes(r, 4);
    return p ? vn_lef32(p) : 0.0f;
}

double vn_rd_f64(vn_rd *r)
{
    uint64_t u = vn_rd_u64(r);
    double d;
    memcpy(&d, &u, 8);
    return d;
}

const char *vn_rd_str(vn_rd *r, uint32_t *len)
{
    uint32_t n = vn_rd_u32(r);
    const uint8_t *p = vn_rd_bytes(r, n);
    *len = p ? n : 0;
    return p ? (const char *)p : "";
}

uint64_t vn_fnv1a64(const void *data, size_t n)
{
    const uint8_t *p = data;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

uint32_t vn_crc32(uint32_t crc, const void *data, size_t n)
{
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    const uint8_t *p = data;
    crc ^= 0xFFFFFFFFu;
    while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t vn_utf8_next(const char *str, size_t len, size_t *pos)
{
    const uint8_t *s = (const uint8_t *)str;
    size_t i = *pos;
    uint8_t c = s[i];
    int n;
    uint32_t cp;
    if (c < 0x80) {
        *pos = i + 1;
        return c;
    } else if ((c & 0xE0) == 0xC0) {
        n = 1;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        n = 2;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        n = 3;
        cp = c & 0x07;
    } else {
        *pos = i + 1;
        return 0xFFFD;
    }
    if (i + (size_t)n >= len) {
        *pos = i + 1;
        return 0xFFFD;
    }
    for (int k = 1; k <= n; k++) {
        if ((s[i + k] & 0xC0) != 0x80) {
            *pos = i + 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (s[i + k] & 0x3F);
    }
    *pos = i + 1 + (size_t)n;
    return cp;
}

size_t vn_utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}
