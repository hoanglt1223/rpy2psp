#include "plat_internal.h"

#include <SDL3/SDL.h>

#define CHUNK_FRAMES 1024
#define FRAME_BYTES ((int)sizeof(int16_t) * PLAT_AUDIO_CHANNELS)

static SDL_AudioStream *stream;
static plat_audio_fn audio_fn;
static void *audio_user;

static void SDLCALL feed(void *userdata, SDL_AudioStream *s, int additional_amount, int total_amount)
{
    (void)userdata;
    (void)total_amount;
    int16_t buf[CHUNK_FRAMES * PLAT_AUDIO_CHANNELS];
    int frames = (additional_amount + FRAME_BYTES - 1) / FRAME_BYTES;
    while (frames > 0) {
        int n = frames < CHUNK_FRAMES ? frames : CHUNK_FRAMES;
        audio_fn(buf, n, audio_user);
        if (!SDL_PutAudioStreamData(s, buf, n * FRAME_BYTES)) return;
        frames -= n;
    }
}

plat_result plat_audio_start(plat_audio_fn fn, void *user)
{
    if (!fn) return PLAT_ERR_INVALID;
    if (stream) return PLAT_ERR_STATE;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        plat_log("plat: audio init failed: %s", SDL_GetError());
        return PLAT_ERR_BACKEND;
    }
    audio_fn = fn;
    audio_user = user;
    const SDL_AudioSpec spec = {SDL_AUDIO_S16, PLAT_AUDIO_CHANNELS, PLAT_AUDIO_RATE};
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, NULL);
    if (!stream) {
        plat_log("plat: SDL_OpenAudioDeviceStream failed: %s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return PLAT_ERR_BACKEND;
    }
    if (!SDL_ResumeAudioStreamDevice(stream)) {
        plat_log("plat: SDL_ResumeAudioStreamDevice failed: %s", SDL_GetError());
        plat_audio_stop();
        return PLAT_ERR_BACKEND;
    }
    plat_log("plat: audio on '%s'", SDL_GetCurrentAudioDriver());
    return PLAT_OK;
}

void plat_audio_stop(void)
{
    if (!stream) return;
    SDL_DestroyAudioStream(stream);
    stream = NULL;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
