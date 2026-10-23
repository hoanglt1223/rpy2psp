/*
 * Platform layer: video frame pacing, input, audio stream, files, clock.
 * Backends: platform/psp (pspsdk, GU) and platform/sdl (SDL3, OpenGL).
 * Application-neutral by design; shared by every PSP port in this workspace.
 *
 * Threading: every function except the audio callback runs on the main thread.
 * Drawing is not part of this API. Between plat_frame_begin and plat_frame_end
 * the caller issues draw calls directly: sceGu* into the platform's GU display
 * list on PSP, OpenGL 2.1 compatibility profile on SDL.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLAT_API_VERSION 1

#define PLAT_SCREEN_W 480
#define PLAT_SCREEN_H 272
#define PLAT_FRAME_HZ 60

/* sceAudio output channels run at a fixed 44.1 kHz, so both backends do. */
#define PLAT_AUDIO_RATE 44100
#define PLAT_AUDIO_CHANNELS 2

#define PLAT_APP_ID_MAX 32
#define PLAT_PATH_MAX 256

typedef enum plat_result {
    PLAT_OK = 0,
    PLAT_ERR_INVALID = -1,   /* bad argument or path */
    PLAT_ERR_STATE = -2,     /* call order violated (not initialised, already running) */
    PLAT_ERR_BACKEND = -3,   /* OS / SDK call failed; details are logged */
    PLAT_ERR_NOT_FOUND = -4,
    PLAT_ERR_IO = -5,
    PLAT_ERR_NO_MEMORY = -6
} plat_result;

typedef struct plat_config {
    const char *app_id;   /* [A-Za-z0-9_-], 1..PLAT_APP_ID_MAX chars; names the user data dir */
    const char *title;    /* window title on PC; may be NULL */
    int window_scale;     /* PC window is 480x272 times this; <= 0 selects 2 */
    bool vsync;
    bool headless;        /* PC only: hidden window and silent dummy audio driver, for CI/smoke runs */
} plat_config;

plat_result plat_init(const plat_config *cfg);
void plat_shutdown(void);

/* Pumps OS events, samples input, binds the frame and clears it to clear_rgb
 * (0xRRGGBB) with stencil 0. Returns false once the user asked to quit; the
 * caller must then skip plat_frame_end and shut down. */
bool plat_frame_begin(uint32_t clear_rgb);
/* Submits the frame and flips. With vsync on it waits for vertical blank and
 * paces to PLAT_FRAME_HZ, so PC monitors faster than 60 Hz behave like the PSP. */
void plat_frame_end(void);
void plat_set_vsync(bool on);

/* PSP button names; the SDL backend maps keyboard and gamepad onto them. */
enum {
    PLAT_BTN_UP = 1u << 0,
    PLAT_BTN_DOWN = 1u << 1,
    PLAT_BTN_LEFT = 1u << 2,
    PLAT_BTN_RIGHT = 1u << 3,
    PLAT_BTN_CROSS = 1u << 4,
    PLAT_BTN_CIRCLE = 1u << 5,
    PLAT_BTN_SQUARE = 1u << 6,
    PLAT_BTN_TRIANGLE = 1u << 7,
    PLAT_BTN_L = 1u << 8,
    PLAT_BTN_R = 1u << 9,
    PLAT_BTN_START = 1u << 10,
    PLAT_BTN_SELECT = 1u << 11
};

typedef struct plat_input {
    uint32_t held;      /* PLAT_BTN_* bits down this frame */
    uint32_t pressed;   /* went down since the previous frame */
    uint32_t released;  /* went up since the previous frame */
    float stick_x;      /* analog nub, -1..1 after dead zone, +x right */
    float stick_y;      /* -1..1, +y down */
} plat_input;

/* Input as sampled by the last plat_frame_begin. */
void plat_input_get(plat_input *out);

/* Fills frames * PLAT_AUDIO_CHANNELS interleaved signed 16-bit samples.
 * Runs on the audio thread: it must not call other plat_* functions. */
typedef void (*plat_audio_fn)(int16_t *out, int frames, void *user);

plat_result plat_audio_start(plat_audio_fn fn, void *user);
void plat_audio_stop(void);

/* DATA is the read-only app directory (EBOOT folder / executable folder).
 * USER is writable: ms0:/PSP/SAVEDATA/<APP_ID>/ on PSP, the SDL pref path on PC. */
typedef enum plat_root { PLAT_ROOT_DATA = 0, PLAT_ROOT_USER = 1 } plat_root;

typedef enum plat_open_mode {
    PLAT_OPEN_READ = 0,
    PLAT_OPEN_WRITE = 1   /* create or truncate; USER root only */
} plat_open_mode;

typedef struct plat_file plat_file;

/* Relative paths only: '/' separated, no empty, "." or ".." segments, no
 * backslash or ':' and shorter than PLAT_PATH_MAX. */
bool plat_path_is_valid(const char *rel);

plat_result plat_file_open(plat_root root, const char *rel, plat_open_mode mode, plat_file **out);
/* Both return bytes transferred (short only at end of file for reads) or a negative plat_result. */
int64_t plat_file_read(plat_file *f, void *buf, size_t size);
int64_t plat_file_write(plat_file *f, const void *buf, size_t size);
plat_result plat_file_seek(plat_file *f, int64_t offset);   /* absolute */
int64_t plat_file_size(plat_file *f);
/* Reports write-back failures, so check it after writing. */
plat_result plat_file_close(plat_file *f);

/* Whole-file helpers. read_all returns a malloc'd buffer (free with free()),
 * NUL-terminated past *size for convenience. write_all goes through a temporary
 * file and a rename so a crash never leaves a truncated file behind. */
plat_result plat_file_read_all(plat_root root, const char *rel, void **data, size_t *size);
plat_result plat_file_write_all(plat_root root, const char *rel, const void *data, size_t size);
plat_result plat_file_remove(plat_root root, const char *rel);
plat_result plat_mkdir(plat_root root, const char *rel);   /* OK if it already exists */

/* Monotonic microseconds since an arbitrary start point. */
uint64_t plat_time_us(void);

void plat_log(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

#ifdef __cplusplus
}
#endif

#endif
