#include "plat_internal.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <stdarg.h>

static SDL_Window *window;
static SDL_GLContext gl_context;
static SDL_Gamepad *gamepad;
static plat_input input_state;
static bool quit_requested;
static bool in_frame;
static bool vsync_on;
static Uint64 next_frame_ns;

static void open_first_gamepad(void)
{
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (!ids) return;
    for (int i = 0; i < count && !gamepad; i++) gamepad = SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
}

plat_result plat_init(const plat_config *cfg)
{
    if (window) return PLAT_ERR_STATE;
    if (!cfg || !plat__app_id_is_valid(cfg->app_id)) return PLAT_ERR_INVALID;

    plat_result r = plat__files_init(cfg->app_id);
    if (r != PLAT_OK) return r;

    /* Audio opens later in plat_audio_start; the hint must be set before then. */
    if (cfg->headless) SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        plat_log("plat: SDL_Init failed: %s", SDL_GetError());
        plat__files_shutdown();
        return PLAT_ERR_BACKEND;
    }

    /* Stencil matches the PSP 8888 framebuffer, which renderers use for masks. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    int scale = cfg->window_scale > 0 ? cfg->window_scale : 2;
    window = SDL_CreateWindow(cfg->title ? cfg->title : cfg->app_id, PLAT_SCREEN_W * scale,
                              PLAT_SCREEN_H * scale,
                              SDL_WINDOW_OPENGL | (cfg->headless ? SDL_WINDOW_HIDDEN : 0));
    if (!window) {
        plat_log("plat: SDL_CreateWindow failed: %s", SDL_GetError());
        goto fail;
    }
    gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        plat_log("plat: SDL_GL_CreateContext failed: %s", SDL_GetError());
        goto fail;
    }
    plat_set_vsync(cfg->vsync);
    open_first_gamepad();

    quit_requested = false;
    in_frame = false;
    SDL_zero(input_state);
    plat_log("plat: SDL %d.%d.%d, GL %s (%s)", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
             SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()),
             (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER));
    return PLAT_OK;

fail:
    if (window) SDL_DestroyWindow(window);
    window = NULL;
    SDL_Quit();
    plat__files_shutdown();
    return PLAT_ERR_BACKEND;
}

void plat_shutdown(void)
{
    plat_audio_stop();
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad = NULL;
    if (gl_context) SDL_GL_DestroyContext(gl_context);
    gl_context = NULL;
    if (window) SDL_DestroyWindow(window);
    window = NULL;
    SDL_Quit();
    plat__files_shutdown();
}

void plat_set_vsync(bool on)
{
    if (!gl_context) return;
    vsync_on = on;
    next_frame_ns = SDL_GetTicksNS();
    if (!SDL_GL_SetSwapInterval(on ? 1 : 0))
        plat_log("plat: SDL_GL_SetSwapInterval(%d) failed: %s", on ? 1 : 0, SDL_GetError());
}

static void handle_event(const SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        quit_requested = true;
        break;
    case SDL_EVENT_KEY_DOWN:
        if (ev->key.key == SDLK_ESCAPE) quit_requested = true;
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!gamepad) gamepad = SDL_OpenGamepad(ev->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (gamepad && SDL_GetGamepadID(gamepad) == ev->gdevice.which) {
            SDL_CloseGamepad(gamepad);
            gamepad = NULL;
            open_first_gamepad();
        }
        break;
    default:
        break;
    }
}

static void sample_input(void)
{
    static const struct { SDL_Scancode key; uint32_t btn; } keymap[] = {
        {SDL_SCANCODE_UP, PLAT_BTN_UP},       {SDL_SCANCODE_DOWN, PLAT_BTN_DOWN},
        {SDL_SCANCODE_LEFT, PLAT_BTN_LEFT},   {SDL_SCANCODE_RIGHT, PLAT_BTN_RIGHT},
        {SDL_SCANCODE_Z, PLAT_BTN_CROSS},     {SDL_SCANCODE_X, PLAT_BTN_CIRCLE},
        {SDL_SCANCODE_A, PLAT_BTN_SQUARE},    {SDL_SCANCODE_S, PLAT_BTN_TRIANGLE},
        {SDL_SCANCODE_Q, PLAT_BTN_L},         {SDL_SCANCODE_W, PLAT_BTN_R},
        {SDL_SCANCODE_RETURN, PLAT_BTN_START}, {SDL_SCANCODE_RSHIFT, PLAT_BTN_SELECT},
    };
    static const struct { SDL_GamepadButton pad; uint32_t btn; } padmap[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, PLAT_BTN_UP},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PLAT_BTN_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PLAT_BTN_LEFT},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PLAT_BTN_RIGHT},
        {SDL_GAMEPAD_BUTTON_SOUTH, PLAT_BTN_CROSS},
        {SDL_GAMEPAD_BUTTON_EAST, PLAT_BTN_CIRCLE},
        {SDL_GAMEPAD_BUTTON_WEST, PLAT_BTN_SQUARE},
        {SDL_GAMEPAD_BUTTON_NORTH, PLAT_BTN_TRIANGLE},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PLAT_BTN_L},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PLAT_BTN_R},
        {SDL_GAMEPAD_BUTTON_START, PLAT_BTN_START},
        {SDL_GAMEPAD_BUTTON_BACK, PLAT_BTN_SELECT},
    };

    uint32_t held = 0;
    const bool *keys = SDL_GetKeyboardState(NULL);
    for (size_t i = 0; i < SDL_arraysize(keymap); i++)
        if (keys[keymap[i].key]) held |= keymap[i].btn;

    /* IJKL stands in for the analog nub when no gamepad is attached. */
    float sx = (float)keys[SDL_SCANCODE_L] - (float)keys[SDL_SCANCODE_J];
    float sy = (float)keys[SDL_SCANCODE_K] - (float)keys[SDL_SCANCODE_I];

    if (gamepad) {
        for (size_t i = 0; i < SDL_arraysize(padmap); i++)
            if (SDL_GetGamepadButton(gamepad, padmap[i].pad)) held |= padmap[i].btn;
        float gx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        float gy = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        if (gx * gx + gy * gy > sx * sx + sy * sy) {
            sx = gx;
            sy = gy;
        }
    }
    plat__input_update(&input_state, held, SDL_clamp(sx, -1.0f, 1.0f), SDL_clamp(sy, -1.0f, 1.0f));
}

bool plat_frame_begin(uint32_t clear_rgb)
{
    if (!window) return false;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) handle_event(&ev);
    if (quit_requested) return false;

    sample_input();

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(((clear_rgb >> 16) & 0xFF) / 255.0f, ((clear_rgb >> 8) & 0xFF) / 255.0f,
                 (clear_rgb & 0xFF) / 255.0f, 1.0f);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    in_frame = true;
    return true;
}

void plat_frame_end(void)
{
    if (!in_frame) return;
    in_frame = false;
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) plat_log("plat: GL error 0x%04x during frame", (unsigned)err);
    SDL_GL_SwapWindow(window);

    if (!vsync_on) return;
    const Uint64 period = SDL_NS_PER_SECOND / PLAT_FRAME_HZ;
    Uint64 now = SDL_GetTicksNS();
    next_frame_ns += period;
    if (next_frame_ns > now) {
        SDL_DelayPrecise(next_frame_ns - now);
    } else if (now - next_frame_ns > period) {
        next_frame_ns = now;   /* fell behind (stall, debugger); do not try to catch up */
    }
}

void plat_input_get(plat_input *out)
{
    if (out) *out = input_state;
}

uint64_t plat_time_us(void)
{
    return SDL_GetTicksNS() / 1000u;
}

void plat_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_LogMessageV(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO, fmt, ap);
    va_end(ap);
}
