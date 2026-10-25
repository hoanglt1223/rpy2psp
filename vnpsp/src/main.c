/* vnpsp entry point.
 * PSP: plays script.vnb + data.pak from the EBOOT folder; saves go to
 *      ms0:/PSP/SAVEDATA/<EBOOT folder name>/.
 * PC:  vnpsp <game dir> [--save-dir DIR] [--scale N] [--no-audio]
 *      vnpsp <game dir> --headless --frames N        platform smoke run: hidden window, silent audio driver
 *      vnpsp <game dir> --play SCRIPT [--trace FILE] [--fast] [--render-all] [--audio] [--wav FILE]
 *                       [--max-frames N] [--texture-budget BYTES] [--save-dir DIR]
 *                                                      scripted run with no platform layer (driver.c) */
#include "platform.h"

#include "base.h"
#include "engine.h"
#include "gfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __PSP__
#include <pspkernel.h>
#include <unistd.h>
PSP_MODULE_INFO("vnpsp", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);
#else
#include "driver.h"
#include "gfx_soft.h"
#include <SDL3/SDL_opengl.h>
#endif

/* Audio thread cost, read by the main loop for the periodic performance log. A torn
 * read only skews one log line. */
static volatile uint32_t audio_busy_us, audio_frames_done;

static void audio_cb(int16_t *out, int frames, void *user)
{
    uint64_t t0 = plat_time_us();
    mixer_render(user, out, frames);
    audio_busy_us += (uint32_t)(plat_time_us() - t0);
    audio_frames_done += (uint32_t)frames;
}

/* Every 5 s: frame CPU time and the share of real time the mixer (MP3 decoding
 * included) needs. */
typedef struct perf_log {
    uint64_t start, frame_us, frame_max;
    uint32_t frames, audio_us0, audio_frames0;
} perf_log;

static void perf_frame(perf_log *p, uint64_t t0)
{
    uint64_t now = plat_time_us(), spent = now - t0;
    if (!p->start) p->start = t0;
    p->frame_us += spent;
    if (spent > p->frame_max) p->frame_max = spent;
    if (++p->frames < 300) return;
    uint32_t a_us = audio_busy_us, a_frames = audio_frames_done;
    uint32_t mixed = a_frames - p->audio_frames0;
    double audio_pct = mixed ? 100.0 * (double)(a_us - p->audio_us0) / (mixed * 1e6 / 44100.0) : 0.0;
    plat_log("vnpsp: perf: %.1f fps, frame cpu %llu us avg / %llu us max, audio mix %.1f%% of real time",
             p->frames * 1e6 / (double)(now - p->start), (unsigned long long)(p->frame_us / p->frames),
             (unsigned long long)p->frame_max, audio_pct);
    *p = (perf_log){.start = now, .audio_us0 = a_us, .audio_frames0 = a_frames};
}

static void to_input(const plat_input *p, vn_input *in)
{
    in->held = p->held;
    in->pressed = p->pressed;
    in->stick_x = p->stick_x;
    in->stick_y = p->stick_y;
}

static void plat_sink(const char *line)
{
    plat_log("%s", line);
}

#ifdef __PSP__

/* The EBOOT folder name, reduced to the characters an app id may use. */
static void app_id_from_cwd(char *out, size_t cap)
{
    char cwd[256];
    const char *base = "VNPSP";
    if (getcwd(cwd, sizeof cwd)) {
        size_t n = strlen(cwd);
        while (n && cwd[n - 1] == '/') cwd[--n] = 0;
        const char *slash = strrchr(cwd, '/');
        if (slash && slash[1]) base = slash + 1;
    }
    size_t k = 0;
    for (const char *p = base; *p && k + 1 < cap && k < PLAT_APP_ID_MAX; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
            out[k++] = c;
    }
    if (!k) k = (size_t)snprintf(out, cap, "VNPSP");
    out[k] = 0;
}

int main(void)
{
    char app_id[PLAT_APP_ID_MAX + 1];
    app_id_from_cwd(app_id, sizeof app_id);
    plat_config pc = {app_id, "vnpsp", 1, true, false};
    if (plat_init(&pc) != PLAT_OK) {
        sceKernelExitGame();
        return 1;
    }
    vn_set_log_sink(plat_sink);
    gfx_init();
    char save_dir[PLAT_PATH_MAX];
    snprintf(save_dir, sizeof save_dir, "ms0:/PSP/SAVEDATA/%s", app_id);
    vn_config cfg = {"", save_dir, false, 12u << 20, NULL, NULL, plat_time_us};
    char err[256];
    vn_engine *e = vn_engine_new(&cfg, err, sizeof err);
    if (!e) {
        plat_log("vnpsp: %s", err);
        /* Keep the message on screen until the user leaves through HOME. */
        while (plat_frame_begin(0x400000)) plat_frame_end();
        plat_shutdown();
        sceKernelExitGame();
        return 1;
    }
    if (plat_audio_start(audio_cb, vn_engine_mixer(e)) != PLAT_OK) plat_log("vnpsp: running without audio");
    perf_log perf = {0};
    while (plat_frame_begin(0)) {
        uint64_t t0 = plat_time_us();
        plat_input pi;
        vn_input in;
        plat_input_get(&pi);
        to_input(&pi, &in);
        vn_engine_update(e, &in, 1.0f / 60.0f);
        vn_engine_render(e);
        perf_frame(&perf, t0);
        plat_frame_end();
        if (vn_engine_mode(e) == MODE_QUIT) break;
    }
    plat_audio_stop();
    vn_engine_free(e);
    gfx_shutdown();
    plat_shutdown();
    sceKernelExitGame();
    return 0;
}

#else

static GLuint screen_tex;

static void blit_frame(void)
{
    if (!screen_tex) {
        glGenTextures(1, &screen_tex);
        glBindTexture(GL_TEXTURE_2D, screen_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    glBindTexture(GL_TEXTURE_2D, screen_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GFX_W, GFX_H, GL_RGBA, GL_UNSIGNED_BYTE, gfx_soft_pixels());
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 1, 1, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glColor4f(1, 1, 1, 1);
    float u = GFX_W / 512.0f, v = GFX_H / 512.0f;
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(0, 0);
    glTexCoord2f(u, 0); glVertex2f(1, 0);
    glTexCoord2f(u, v); glVertex2f(1, 1);
    glTexCoord2f(0, v); glVertex2f(0, 1);
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

static int usage(void)
{
    fprintf(stderr,
            "usage: vnpsp <game dir> [--save-dir DIR] [--scale N] [--no-audio]\n"
            "       vnpsp <game dir> --headless --frames N\n"
            "       vnpsp <game dir> --play SCRIPT [--trace FILE] [--fast] [--render-all] [--audio]\n"
            "                        [--wav FILE] [--max-frames N] [--texture-budget BYTES] [--save-dir DIR]\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *game = NULL, *save_dir = NULL, *play = NULL, *trace = NULL;
    driver_opts dopts = {0};
    bool headless = false, no_audio = false;
    long frames = 0;
    int scale = 2;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool more = i + 1 < argc;
        if (strcmp(a, "--save-dir") == 0 && more) save_dir = argv[++i];
        else if (strcmp(a, "--play") == 0 && more) play = argv[++i];
        else if (strcmp(a, "--trace") == 0 && more) trace = argv[++i];
        else if (strcmp(a, "--wav") == 0 && more) dopts.wav_path = argv[++i];
        else if (strcmp(a, "--max-frames") == 0 && more) dopts.max_frames = strtol(argv[++i], NULL, 10);
        else if (strcmp(a, "--texture-budget") == 0 && more) dopts.texture_budget = (size_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(a, "--frames") == 0 && more) frames = strtol(argv[++i], NULL, 10);
        else if (strcmp(a, "--scale") == 0 && more) scale = atoi(argv[++i]);
        else if (strcmp(a, "--fast") == 0) dopts.fast = true;
        else if (strcmp(a, "--render-all") == 0) dopts.render_all = true;
        else if (strcmp(a, "--audio") == 0) dopts.audio = true;
        else if (strcmp(a, "--headless") == 0) headless = true;
        else if (strcmp(a, "--no-audio") == 0) no_audio = true;
        else if (a[0] != '-' && !game) game = a;
        else return usage();
    }
    if (!game) return usage();
    char default_saves[512];
    if (!save_dir) {
        vn_path_join(default_saves, sizeof default_saves, game, "saves");
        save_dir = default_saves;
    }
    if (play) return driver_main(game, save_dir, play, trace, &dopts);
    if (headless && frames <= 0) return usage();

    plat_config pc = {"vnpsp", "vnpsp", scale, true, headless};
    if (plat_init(&pc) != PLAT_OK) {
        fprintf(stderr, "vnpsp: platform init failed\n");
        return 1;
    }
    vn_set_log_sink(plat_sink);
    gfx_init();
    vn_config cfg = {game, save_dir, headless, 12u << 20, NULL, NULL, plat_time_us};
    char err[256];
    vn_engine *e = vn_engine_new(&cfg, err, sizeof err);
    if (!e) {
        plat_log("vnpsp: %s", err);
        plat_shutdown();
        return 1;
    }
    if (!no_audio && plat_audio_start(audio_cb, vn_engine_mixer(e)) != PLAT_OK) plat_log("vnpsp: running without audio");
    long n = 0;
    int rc = 0;
    perf_log perf = {0};
    while (plat_frame_begin(0)) {
        uint64_t t0 = plat_time_us();
        plat_input pi;
        vn_input in;
        plat_input_get(&pi);
        to_input(&pi, &in);
        if (headless) {
            /* Smoke run: start the game and keep pressing X so the story moves. */
            memset(&in, 0, sizeof in);
            if (n % 20 == 10) in.pressed = in.held = VN_CROSS;
        }
        vn_engine_update(e, &in, 1.0f / 60.0f);
        vn_engine_render(e);
        blit_frame();
        perf_frame(&perf, t0);
        plat_frame_end();
        if (vn_engine_mode(e) == MODE_QUIT) break;
        if (vn_engine_mode(e) == MODE_ERROR && headless) {
            plat_log("vnpsp: %s", vn_engine_error(e));
            rc = 1;
            break;
        }
        if (headless && ++n >= frames) break;
    }
    if (headless) plat_log("vnpsp: smoke run finished after %ld frames in mode %d", n, (int)vn_engine_mode(e));
    plat_audio_stop();
    vn_engine_free(e);
    gfx_shutdown();
    plat_shutdown();
    return rc;
}

#endif
