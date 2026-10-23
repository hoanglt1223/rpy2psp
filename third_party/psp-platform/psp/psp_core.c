#include "plat_internal.h"

#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <psppower.h>

#include <stdarg.h>
#include <stdio.h>

#define BUF_WIDTH 512

/* GE display list for one frame; renderers allocate vertices from it with sceGuGetMemory. */
static unsigned int __attribute__((aligned(64))) gu_list[256 * 1024 / sizeof(unsigned int)];

static volatile int quit_requested;
static bool initialised;
static bool in_frame;
static bool vsync_on;
static plat_input input_state;

static int exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1;
    (void)arg2;
    (void)common;
    quit_requested = 1;
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    int cbid = sceKernelCreateCallback("plat_exit_cb", exit_callback, NULL);
    if (cbid >= 0) sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static void gu_setup(void)
{
    void *fbp0 = guGetStaticVramBuffer(BUF_WIDTH, PLAT_SCREEN_H, GU_PSM_8888);
    void *fbp1 = guGetStaticVramBuffer(BUF_WIDTH, PLAT_SCREEN_H, GU_PSM_8888);
    /* Depth is never tested or written (2D only), but the GE still wants a valid buffer. */
    void *zbp = guGetStaticVramBuffer(BUF_WIDTH, PLAT_SCREEN_H, GU_PSM_4444);

    sceGuInit();
    sceGuStart(GU_DIRECT, gu_list);
    sceGuDrawBuffer(GU_PSM_8888, fbp0, BUF_WIDTH);
    sceGuDispBuffer(PLAT_SCREEN_W, PLAT_SCREEN_H, fbp1, BUF_WIDTH);
    sceGuDepthBuffer(zbp, BUF_WIDTH);
    sceGuOffset(2048 - (PLAT_SCREEN_W / 2), 2048 - (PLAT_SCREEN_H / 2));
    sceGuViewport(2048, 2048, PLAT_SCREEN_W, PLAT_SCREEN_H);
    sceGuDepthRange(65535, 0);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);
    sceGuScissor(0, 0, PLAT_SCREEN_W, PLAT_SCREEN_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuFrontFace(GU_CW);
    sceGuShadeModel(GU_SMOOTH);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

plat_result plat_init(const plat_config *cfg)
{
    if (initialised) return PLAT_ERR_STATE;
    if (!cfg || !plat__app_id_is_valid(cfg->app_id)) return PLAT_ERR_INVALID;

    SceUID thid = sceKernelCreateThread("plat_cb", callback_thread, 0x11, 0x1000, PSP_THREAD_ATTR_USER, NULL);
    if (thid < 0) {
        plat_log("plat: callback thread create failed 0x%08x", (unsigned)thid);
        return PLAT_ERR_BACKEND;
    }
    sceKernelStartThread(thid, 0, NULL);

    int pr = scePowerSetClockFrequency(333, 333, 166);
    if (pr < 0) plat_log("plat: scePowerSetClockFrequency failed 0x%08x", (unsigned)pr);

    plat_result r = plat__files_init(cfg->app_id);
    if (r != PLAT_OK) return r;

    gu_setup();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    vsync_on = cfg->vsync;
    quit_requested = 0;
    in_frame = false;
    input_state = (plat_input){0};
    initialised = true;
    plat_log("plat: PSP %d MHz", scePowerGetCpuClockFrequency());
    return PLAT_OK;
}

void plat_shutdown(void)
{
    if (!initialised) return;
    plat_audio_stop();
    sceGuTerm();
    plat__files_shutdown();
    initialised = false;
}

void plat_set_vsync(bool on)
{
    vsync_on = on;
}

static void sample_input(void)
{
    static const struct { unsigned int psp; uint32_t btn; } map[] = {
        {PSP_CTRL_UP, PLAT_BTN_UP},         {PSP_CTRL_DOWN, PLAT_BTN_DOWN},
        {PSP_CTRL_LEFT, PLAT_BTN_LEFT},     {PSP_CTRL_RIGHT, PLAT_BTN_RIGHT},
        {PSP_CTRL_CROSS, PLAT_BTN_CROSS},   {PSP_CTRL_CIRCLE, PLAT_BTN_CIRCLE},
        {PSP_CTRL_SQUARE, PLAT_BTN_SQUARE}, {PSP_CTRL_TRIANGLE, PLAT_BTN_TRIANGLE},
        {PSP_CTRL_LTRIGGER, PLAT_BTN_L},    {PSP_CTRL_RTRIGGER, PLAT_BTN_R},
        {PSP_CTRL_START, PLAT_BTN_START},   {PSP_CTRL_SELECT, PLAT_BTN_SELECT},
    };
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(&pad, 1) < 0) {
        plat__input_update(&input_state, 0, 0.0f, 0.0f);
        return;
    }
    uint32_t held = 0;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (pad.Buttons & map[i].psp) held |= map[i].btn;
    float sx = ((int)pad.Lx - 128) / 127.0f;
    float sy = ((int)pad.Ly - 128) / 127.0f;
    if (sx < -1.0f) sx = -1.0f;
    if (sy < -1.0f) sy = -1.0f;
    plat__input_update(&input_state, held, sx, sy);
}

bool plat_frame_begin(uint32_t clear_rgb)
{
    if (!initialised || quit_requested) return false;
    sample_input();

    uint32_t r = (clear_rgb >> 16) & 0xFF, g = (clear_rgb >> 8) & 0xFF, b = clear_rgb & 0xFF;
    sceGuStart(GU_DIRECT, gu_list);
    sceGuClearColor(0xFF000000u | (b << 16) | (g << 8) | r);
    sceGuClearStencil(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_STENCIL_BUFFER_BIT);
    in_frame = true;
    return true;
}

void plat_frame_end(void)
{
    if (!in_frame) return;
    in_frame = false;
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    if (vsync_on) sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

void plat_input_get(plat_input *out)
{
    if (out) *out = input_state;
}

uint64_t plat_time_us(void)
{
    return (uint64_t)sceKernelGetSystemTimeWide();
}

void plat_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}
