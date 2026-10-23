#include "plat_internal.h"

#include <pspaudio.h>
#include <pspkernel.h>

/* Must be a multiple of 64 for sceAudioChReserve; ~23 ms per buffer. */
#define AUDIO_FRAMES 1024

static int16_t __attribute__((aligned(64))) audio_buf[2][AUDIO_FRAMES * PLAT_AUDIO_CHANNELS];
static volatile int audio_running;
static int audio_ch = -1;
static SceUID audio_thid = -1;
static plat_audio_fn audio_fn;
static void *audio_user;

static int audio_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    int cur = 0;
    while (audio_running) {
        audio_fn(audio_buf[cur], AUDIO_FRAMES, audio_user);
        int r = sceAudioOutputBlocking(audio_ch, PSP_AUDIO_VOLUME_MAX, audio_buf[cur]);
        if (r < 0) {
            plat_log("plat: sceAudioOutputBlocking failed 0x%08x", (unsigned)r);
            break;
        }
        cur ^= 1;
    }
    return 0;
}

plat_result plat_audio_start(plat_audio_fn fn, void *user)
{
    if (!fn) return PLAT_ERR_INVALID;
    if (audio_ch >= 0) return PLAT_ERR_STATE;

    audio_ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, AUDIO_FRAMES, PSP_AUDIO_FORMAT_STEREO);
    if (audio_ch < 0) {
        plat_log("plat: sceAudioChReserve failed 0x%08x", (unsigned)audio_ch);
        audio_ch = -1;
        return PLAT_ERR_BACKEND;
    }
    audio_fn = fn;
    audio_user = user;
    audio_running = 1;
    /* Higher priority than the main thread (0x20) so rendering never starves playback. */
    audio_thid = sceKernelCreateThread("plat_audio", audio_thread, 0x12, 0x10000,
                                       PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
    if (audio_thid < 0 || sceKernelStartThread(audio_thid, 0, NULL) < 0) {
        plat_log("plat: audio thread start failed 0x%08x", (unsigned)audio_thid);
        if (audio_thid >= 0) sceKernelDeleteThread(audio_thid);
        audio_thid = -1;
        audio_running = 0;
        sceAudioChRelease(audio_ch);
        audio_ch = -1;
        return PLAT_ERR_BACKEND;
    }
    return PLAT_OK;
}

void plat_audio_stop(void)
{
    if (audio_ch < 0) return;
    audio_running = 0;
    if (audio_thid >= 0) {
        sceKernelWaitThreadEnd(audio_thid, NULL);
        sceKernelDeleteThread(audio_thid);
        audio_thid = -1;
    }
    sceAudioChRelease(audio_ch);
    audio_ch = -1;
}
