/* Audio: decoders for MUS1 (MP3 through minimp3, gapless loops) and SND1
 * (PCM16 / IMA ADPCM), and a mixer of the script's channels at 44.1 kHz stereo.
 * The engine (main thread) sends commands; mixer_render runs on the audio
 * thread, or on the main thread in headless runs. One producer, one consumer. */
#ifndef VN_AUDIO_H
#define VN_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#define MIX_RATE 44100
#define MIX_MAX_CHANNELS 32
#define MIX_PLAYLIST_MAX 8

typedef struct vn_source vn_source;

/* Parses an entry (MUS1 or SND1); takes ownership of data. NULL if malformed. */
vn_source *source_new(uint8_t *data, uint32_t size);
void source_free(vn_source *s);
/* Decodes up to frames stereo frames at 44.1 kHz; returns frames written (less at the end). */
int source_read(vn_source *s, int16_t *out, int frames);
void source_rewind(vn_source *s);
/* Source length in 44.1 kHz frames. */
uint32_t source_length(const vn_source *s);

typedef struct vn_mixer vn_mixer;

/* mixer_of[i] is the mixer (MIXER_MUSIC/SFX/VOICE) of channel i. */
vn_mixer *mixer_new(int nchannels, const uint8_t *mixer_of);
void mixer_free(vn_mixer *m);

/* Starts sources[0..n) as a playlist; ownership passes to the mixer. queue
 * appends instead of replacing; the old track fades out over fadeout seconds. */
void mixer_play(vn_mixer *m, int ch, vn_source **sources, int n, bool loop, bool queue, float fadein,
                float fadeout, float volume);
void mixer_stop(vn_mixer *m, int ch, float fadeout);
void mixer_channel_volume(vn_mixer *m, int ch, float volume, float delay);
void mixer_volumes(vn_mixer *m, float music, float sfx, float voice);

/* Applies pending commands and mixes; out holds frames * 2 samples. */
void mixer_render(vn_mixer *m, int16_t *out, int frames);
/* Number of commands that did not fit the queue (a full queue drops the command). */
uint32_t mixer_dropped(const vn_mixer *m);

#endif
