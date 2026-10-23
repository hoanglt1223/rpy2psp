/* Unit tests for the engine core: Python value semantics, serialisation,
 * Python-compatible random numbers, ATL timing, audio decoding (ADPCM and the
 * gapless MP3 trim), and GE-style DXT decoding in the software renderer. */
#include "atl.h"
#include "audio.h"
#include "base.h"
#include "fs.h"
#include "gfx.h"
#include "gfx_soft.h"
#include "value.h"
#include "vm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
            failures++;                                                               \
        }                                                                             \
    } while (0)

static char *repr(value v)
{
    vn_buf b = {0};
    v_repr(v, &b);
    vn_buf_u8(&b, 0);
    return (char *)b.data;
}

static void check_repr(value v, const char *want, int line)
{
    char *got = repr(v);
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "unit_tests.c:%d: repr %s, want %s\n", line, got, want);
        failures++;
    }
    free(got);
    v_dec(v);
}
#define CHECK_REPR(v, want) check_repr((v), (want), __LINE__)

static value bin(int op, value a, value b)
{
    value r = v_none();
    verr e;
    if (!v_binary(op, a, b, &r, &e)) {
        fprintf(stderr, "binary op failed: %s\n", e.msg);
        failures++;
    }
    v_dec(a);
    v_dec(b);
    return r;
}

static void test_values(void)
{
    CHECK_REPR(v_float(0.1 + 0.2), "0.30000000000000004");
    CHECK_REPR(v_float(1e16), "1e+16");
    CHECK_REPR(v_float(1e-5), "1e-05");
    CHECK_REPR(v_float(0.0001), "0.0001");
    CHECK_REPR(v_float(-0.0), "-0.0");
    CHECK_REPR(v_float(2.0), "2.0");
    CHECK_REPR(v_float(123456789.0), "123456789.0");
    CHECK_REPR(bin(BIN_FLOORDIV, v_int(-7), v_int(2)), "-4");
    CHECK_REPR(bin(BIN_MOD, v_int(-7), v_int(3)), "2");
    CHECK_REPR(bin(BIN_MOD, v_int(7), v_int(-3)), "-2");
    CHECK_REPR(bin(BIN_DIV, v_int(7), v_int(2)), "3.5");
    CHECK_REPR(bin(BIN_ADD, v_bool(true), v_int(1)), "2");
    CHECK_REPR(bin(BIN_MUL, v_cstr("ab"), v_int(3)), "'ababab'");
    CHECK_REPR(bin(BIN_MOD, v_cstr("%03d%%"), v_int(7)), "'007%'");
    CHECK_REPR(v_cstr("it's"), "\"it's\"");

    verr e;
    value r;
    value s = v_cstr("h\xc3\xa9llo");   /* "héllo": indexes count characters, not bytes */
    CHECK(v_subscr(s, v_int(1), &r, &e));
    CHECK_REPR(r, "'\xc3\xa9'");
    CHECK(v_slice(s, v_int(1), v_int(-1), &r, &e));
    CHECK_REPR(r, "'\xc3\xa9ll'");
    CHECK(!v_subscr(s, v_int(9), &r, &e));
    CHECK(strstr(e.msg, "IndexError") != NULL);
    v_dec(s);

    value d = v_dict_new(0);
    CHECK(v_dict_set(d, v_cstr("a"), v_int(1), &e));
    CHECK(v_dict_set(d, v_int(1), v_cstr("x"), &e));
    CHECK(v_dict_set(d, v_float(1.0), v_cstr("y"), &e));   /* 1 and 1.0 are the same key */
    CHECK(v_as_dict(d)->n == 2);
    CHECK(!v_dict_set(d, v_list_new(V_LIST, 0), v_none(), &e));
    CHECK(strstr(e.msg, "unhashable") != NULL);
    CHECK_REPR(v_inc(d), "{'a': 1, 1: 'y'}");
    CHECK(!v_subscr(d, v_cstr("zz"), &r, &e));
    CHECK(strstr(e.msg, "KeyError") != NULL);
    v_dec(d);

    vn_buf fb = {0};
    CHECK(v_format(v_float(3.14159), "08.2f", 5, &fb, &e));
    CHECK(v_format(v_int(1234567), ",", 1, &fb, &e));
    CHECK(v_format(v_cstr("ab"), "*^6", 3, &fb, &e));
    vn_buf_u8(&fb, 0);
    CHECK(strcmp((const char *)fb.data, "00003.141,234,567**ab**") == 0);
    vn_buf_free(&fb);

    value args[2] = {v_float(2.5), v_none()};
    CHECK(v_call_pure(FN_ROUND, args, 1, &r, &e));
    CHECK_REPR(r, "2");   /* round half to even */
    args[0] = v_float(-2.5);
    CHECK(v_call_pure(FN_ROUND, args, 1, &r, &e));
    CHECK_REPR(r, "-2");
}

static void test_serialisation_keeps_aliasing(void)
{
    value shared = v_list_new(V_LIST, 0);
    v_list_push(shared, v_int(1));
    value outer = v_list_new(V_TUPLE, 0);
    v_list_push(outer, v_inc(shared));
    v_list_push(outer, v_inc(shared));
    v_list_push(outer, v_cstr("x"));
    vn_buf b = {0};
    vser *s = vser_new(&b);
    vser_value(s, outer);
    vser_free(s);
    vn_rd rd;
    vn_rd_init(&rd, b.data, b.len);
    vdeser *d = vdeser_new(&rd);
    value back;
    CHECK(vdeser_value(d, &back));
    vdeser_free(d);
    CHECK(v_equal(back, outer));
    vlist *t = v_as_list(back);
    CHECK(t->n == 3 && t->items[0].u.o == t->items[1].u.o);   /* still one list */
    v_list_push(t->items[0], v_int(2));
    CHECK(v_as_list(t->items[1])->n == 2);
    /* Truncated input is rejected rather than read past the end. */
    for (size_t cut = 0; cut < b.len; cut++) {
        vn_rd_init(&rd, b.data, cut);
        d = vdeser_new(&rd);
        value x;
        CHECK(!vdeser_value(d, &x));
        vdeser_free(d);
    }
    v_dec(back);
    v_dec(outer);
    v_dec(shared);
    vn_buf_free(&b);
}

static void test_python_random(void)
{
    vm_rng r;
    vm_rng_seed(&r, 0);
    CHECK(vm_rng_u32(&r) == 3626764237u);   /* random.seed(0); random.getrandbits(32) */
    vm_rng_seed(&r, 0);
    uint32_t a = vm_rng_u32(&r) >> 5, b = vm_rng_u32(&r) >> 6;
    double x = (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
    CHECK(x == 0.8444218515250481);         /* random.seed(0); random.random() */
}

static void put_f32(uint8_t *p, float f)
{
    memcpy(p, &f, 4);
}

static void test_atl(void)
{
    /* SET xpos 0.0; INTERP linear 2.0 xpos 1.0; REPEAT -1 */
    uint8_t code[64];
    size_t n = 0;
    code[n++] = 0x01; code[n++] = 1; code[n++] = P_XPOS; code[n++] = 1; code[n++] = AV_FLOAT;
    put_f32(code + n, 0.0f); n += 4;
    code[n++] = 0x02; code[n++] = 0; put_f32(code + n, 2.0f); n += 4;
    code[n++] = 1; code[n++] = P_XPOS; code[n++] = 1; code[n++] = AV_FLOAT; put_f32(code + n, 1.0f); n += 4;
    code[n++] = 0x04; code[n++] = 0xFF; code[n++] = 0xFF; code[n++] = 0xFF; code[n++] = 0xFF;
    vn_atl tr = {code, (uint32_t)n};
    vn_script s;
    memset(&s, 0, sizeof s);
    s.ntransforms = 1;
    s.transforms = &tr;
    atl_env env = {1280, 720, 100, 100, 0, "show"};
    double times[] = {0.0, 0.5, 2.5, 1000.5};
    float want[] = {0.0f, 0.25f, 0.25f, 0.25f};
    for (int i = 0; i < 4; i++) {
        atl_props p;
        atl_props_init(&p);
        CHECK(!atl_apply(&s, 0, times[i], &p, &env));   /* a repeat never finishes */
        CHECK(fabsf(p.v[P_XPOS].v - want[i]) < 1e-5f);
    }
    CHECK(atl_duration(&s, 0, &env) < 0);
    CHECK(fabsf(atl_warp(1, 0.5f) - 0.5f) < 1e-6f);
}

static uint8_t *load_fixture(const char *name, size_t *size)
{
    char path[512];
    vn_path_join(path, sizeof path, VNPSP_FIXTURES, name);
    void *data;
    if (!vn_fs_read_all(path, &data, size)) {
        fprintf(stderr, "missing fixture %s\n", path);
        failures++;
        return NULL;
    }
    return data;
}

static void test_adpcm(void)
{
    size_t n;
    uint8_t *raw = load_fixture("ramp.adpcm", &n);
    if (!raw) return;
    uint8_t *blob = malloc(16 + n);
    memcpy(blob, "SND1", 4);
    uint32_t rate = 44100, frames = 1024;
    memcpy(blob + 4, &rate, 4);
    blob[8] = 1;
    blob[9] = 1;
    uint16_t block = 1024;
    memcpy(blob + 10, &block, 2);
    memcpy(blob + 12, &frames, 4);
    memcpy(blob + 16, raw, n);
    free(raw);
    vn_source *s = source_new(blob, (uint32_t)(16 + n));
    CHECK(s != NULL);
    if (!s) return;
    int16_t out[2 * 1100];
    int got = source_read(s, out, 1100);
    CHECK(got == 1024);
    /* Values from rpy2psp.assets.audio.decode_adpcm on the same data. */
    const int16_t first[8] = {-11, -41, -104, -240, -533, -1164, -2521, -5043};
    for (int i = 0; i < 8; i++) CHECK(out[2 * i] == first[i] && out[2 * i + 1] == first[i]);
    const int16_t last[4] = {15402, 15419, 15440, 15458};
    for (int i = 0; i < 4; i++) CHECK(out[2 * (1020 + i)] == last[i]);
    source_free(s);
}

static void test_mp3_trim(void)
{
    size_t n;
    uint8_t *blob = load_fixture("sine441.mus", &n);
    if (!blob) return;
    vn_source *s = source_new(blob, (uint32_t)n);
    CHECK(s != NULL);
    if (!s) return;
    CHECK(source_length(s) == 22050);
    int16_t *pcm = malloc(sizeof(int16_t) * 2 * 30000);
    int got = source_read(s, pcm, 30000);
    CHECK(got == 22050);   /* exactly the source length: encoder delay and padding are dropped */
    /* After the trim the tone starts at phase 0: correlate against sin and cos. */
    double sn = 0, cs = 0;
    for (int i = 0; i < 4410; i++) {
        double ph = 2.0 * 3.14159265358979 * i / 100.0;
        sn += pcm[2 * i] * sin(ph);
        cs += pcm[2 * i] * cos(ph);
    }
    CHECK(sn > 0 && fabs(cs) < 0.05 * sn);
    /* Rewinding replays the same samples (the gapless loop). */
    source_rewind(s);
    int16_t *again = malloc(sizeof(int16_t) * 2 * 22050);
    CHECK(source_read(s, again, 22050) == 22050);
    CHECK(memcmp(pcm, again, sizeof(int16_t) * 2 * 22050) == 0);
    free(again);
    free(pcm);
    source_free(s);
}

static void test_dxt_like_the_ge(void)
{
    gfx_init();
    /* One DXT1 block in the GE's layout: index rows first, then color0, color1.
     * Row 0 uses index 0 (pure red), row 1 index 1 (pure blue), row 2 index 2. */
    uint8_t block[8] = {0x00, 0x55, 0xAA, 0x00, 0x00, 0xF8, 0x1F, 0x00};
    static _Alignas(16) uint8_t data[16];
    memcpy(data, block, 8);
    gfx_tex_desc d = {GFX_DXT1, false, 4, 4, 2, 2, 4, data, 8};
    gfx_tex *t = gfx_tex_create(&d);
    CHECK(t != NULL);
    gfx_frame_begin(gfx_rgba(0, 0, 0, 255));
    float xy[8] = {0, 0, 4, 0, 4, 4, 0, 4}, uv[8] = {0, 0, 4, 0, 4, 4, 0, 4};
    gfx_draw_quad(t, xy, uv, 0xFFFFFFFFu, GFX_NEAREST, NULL);
    gfx_frame_end();
    const uint32_t *px = gfx_soft_pixels();
    CHECK(px[0] == gfx_rgba(248, 0, 0, 255));              /* 31 << 3, no low-bit replication */
    CHECK(px[GFX_W] == gfx_rgba(0, 0, 248, 255));
    CHECK(px[2 * GFX_W] == gfx_rgba(165, 0, 82, 255));     /* (2 * c0 + c1) / 3 */
    gfx_tex_destroy(t);
    gfx_shutdown();
}

int main(void)
{
    test_values();
    test_serialisation_keeps_aliasing();
    test_python_random();
    test_atl();
    test_adpcm();
    test_mp3_trim();
    test_dxt_like_the_ge();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all unit tests passed\n");
    return 0;
}
