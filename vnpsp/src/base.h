/* Shared helpers: logging, growable byte buffer, byte readers, hashes, UTF-8. */
#ifndef VN_BASE_H
#define VN_BASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VN_NONE16 0xFFFFu
#define VN_NONE32 0xFFFFFFFFu
#define VN_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#ifdef __GNUC__
#define VN_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define VN_PRINTF(a, b)
#endif

void vn_log(const char *fmt, ...) VN_PRINTF(1, 2);
/* Route log lines elsewhere (the PSP build sends them to plat_log). */
void vn_set_log_sink(void (*sink)(const char *line));

/* Aborts on allocation failure: the engine cannot continue without memory and
 * every caller would otherwise need the same error path. */
void *vn_alloc(size_t size);
void *vn_calloc(size_t n, size_t size);
void *vn_realloc(void *p, size_t size);
char *vn_strdup(const char *s);

typedef struct vn_buf {
    uint8_t *data;
    size_t len, cap;
} vn_buf;

void vn_buf_reserve(vn_buf *b, size_t extra);
void vn_buf_put(vn_buf *b, const void *data, size_t n);
void vn_buf_u8(vn_buf *b, uint8_t v);
void vn_buf_u16(vn_buf *b, uint16_t v);
void vn_buf_u32(vn_buf *b, uint32_t v);
void vn_buf_u64(vn_buf *b, uint64_t v);
void vn_buf_f64(vn_buf *b, double v);
void vn_buf_str(vn_buf *b, const char *s, size_t n);   /* u32 length + bytes */
void vn_buf_free(vn_buf *b);

/* Bounds-checked little-endian reader; any overrun sets `bad` and returns zeros. */
typedef struct vn_rd {
    const uint8_t *p;
    size_t len, pos;
    bool bad;
} vn_rd;

void vn_rd_init(vn_rd *r, const void *data, size_t len);
uint8_t vn_rd_u8(vn_rd *r);
uint16_t vn_rd_u16(vn_rd *r);
uint32_t vn_rd_u32(vn_rd *r);
int32_t vn_rd_i32(vn_rd *r);
uint64_t vn_rd_u64(vn_rd *r);
float vn_rd_f32(vn_rd *r);
double vn_rd_f64(vn_rd *r);
const uint8_t *vn_rd_bytes(vn_rd *r, size_t n);
/* u32 length + bytes; returns a pointer into the buffer (not NUL-terminated). */
const char *vn_rd_str(vn_rd *r, uint32_t *len);

static inline uint16_t vn_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t vn_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
float vn_lef32(const uint8_t *p);

uint64_t vn_fnv1a64(const void *data, size_t n);
uint32_t vn_crc32(uint32_t crc, const void *data, size_t n);

/* Decodes one code point; invalid bytes yield U+FFFD and advance by one. */
uint32_t vn_utf8_next(const char *s, size_t len, size_t *pos);
size_t vn_utf8_encode(uint32_t cp, char out[4]);

static inline float vn_clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

#endif
