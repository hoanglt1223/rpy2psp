#include "audio.h"

#include "base.h"
#include "vnb.h"

#include <stdlib.h>
#include <string.h>

#define MINIMP3_ONLY_MP3
#if defined(__PSP__)
#define MINIMP3_NO_SIMD
#endif
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#if defined(__PSP__) || defined(__STDC_NO_ATOMICS__)
/* Single in-order core: aligned word loads/stores are atomic; only the compiler may reorder. */
typedef volatile uint32_t ring_index;
static inline uint32_t ring_load(ring_index *x)
{
    __asm__ volatile("" ::: "memory");
    return *x;
}
#define RING_LOAD(x) ring_load(&(x))
#define RING_STORE(x, v) do { __asm__ volatile("" ::: "memory"); (x) = (v); } while (0)
#else
#include <stdatomic.h>
typedef _Atomic uint32_t ring_index;
#define RING_LOAD(x) atomic_load_explicit(&(x), memory_order_acquire)
#define RING_STORE(x, v) atomic_store_explicit(&(x), (v), memory_order_release)
#endif

enum { SRC_MP3, SRC_PCM, SRC_ADPCM };
#define ADPCM_BLOCK 1024

static const int16_t ima_steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
    97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
    4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t ima_adj[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

struct vn_source {
    uint8_t *data;
    uint32_t size;
    int kind, channels;
    uint32_t rate, frames;     /* source rate and length in source frames */
    uint32_t body;             /* offset of MP3 stream / sample payload */
    /* MP3 */
    mp3dec_t dec;
    uint32_t pos, delay, skip, remaining;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int pcm_frames, pcm_pos;
    /* PCM / ADPCM */
    uint64_t out_index;
    int32_t block_index;
    int16_t block[ADPCM_BLOCK * 2];
};

void source_free(vn_source *s)
{
    if (!s) return;
    free(s->data);
    free(s);
}

void source_rewind(vn_source *s)
{
    if (s->kind == SRC_MP3) {
        mp3dec_init(&s->dec);
        s->pos = s->body;
        s->skip = s->delay;
        s->remaining = s->frames;
        s->pcm_frames = s->pcm_pos = 0;
    } else {
        s->out_index = 0;
        s->block_index = -1;
    }
}

vn_source *source_new(uint8_t *data, uint32_t size)
{
    vn_source *s = vn_calloc(1, sizeof *s);
    s->data = data;
    s->size = size;
    if (size >= 16 && memcmp(data, "MUS1", 4) == 0) {
        s->kind = SRC_MP3;
        s->frames = vn_le32(data + 4);
        s->delay = vn_le16(data + 8);
        s->channels = data[10];
        s->rate = vn_le32(data + 12);
        s->body = 16;
        if (s->rate != MIX_RATE || s->channels < 1 || s->channels > 2) goto bad;
    } else if (size >= 16 && memcmp(data, "SND1", 4) == 0) {
        s->rate = vn_le32(data + 4);
        s->channels = data[8];
        s->kind = data[9] == 0 ? SRC_PCM : data[9] == 1 ? SRC_ADPCM : -1;
        s->frames = vn_le32(data + 12);
        s->body = 16;
        if (s->kind < 0 || s->channels < 1 || s->channels > 2 || vn_le16(data + 10) != ADPCM_BLOCK ||
            (s->rate != 11025 && s->rate != 22050 && s->rate != 44100 && s->rate != 48000))
            goto bad;
        uint64_t need = s->kind == SRC_PCM
                            ? (uint64_t)s->frames * s->channels * 2
                            : (uint64_t)((s->frames + ADPCM_BLOCK - 1) / ADPCM_BLOCK) * s->channels * (4 + ADPCM_BLOCK / 2);
        if (need > size - 16) goto bad;
    } else {
        goto bad;
    }
    source_rewind(s);
    return s;
bad:
    vn_log("audio: malformed audio entry");
    source_free(s);
    return NULL;
}

uint32_t source_length(const vn_source *s)
{
    return (uint32_t)((uint64_t)s->frames * MIX_RATE / s->rate);
}

static int mp3_read(vn_source *s, int16_t *out, int frames)
{
    int done = 0;
    while (done < frames && s->remaining > 0) {
        if (s->pcm_pos >= s->pcm_frames) {
            if (s->pos >= s->size) break;
            mp3dec_frame_info_t info;
            int n = mp3dec_decode_frame(&s->dec, s->data + s->pos, (int)(s->size - s->pos), s->pcm, &info);
            if (info.frame_bytes <= 0) break;
            s->pos += (uint32_t)info.frame_bytes;
            if (n <= 0) continue;
            if (info.channels != s->channels) {
                /* A stream that changes channel count mid-way is not one the pipeline writes. */
                s->remaining = 0;
                break;
            }
            s->pcm_frames = n;
            s->pcm_pos = 0;
        }
        int avail = s->pcm_frames - s->pcm_pos;
        if (s->skip) {
            int k = (uint32_t)avail < s->skip ? avail : (int)s->skip;
            s->skip -= (uint32_t)k;
            s->pcm_pos += k;
            continue;
        }
        int k = frames - done;
        if (k > avail) k = avail;
        if ((uint32_t)k > s->remaining) k = (int)s->remaining;
        for (int i = 0; i < k; i++) {
            const int16_t *p = s->pcm + (s->pcm_pos + i) * s->channels;
            out[2 * (done + i)] = p[0];
            out[2 * (done + i) + 1] = s->channels == 2 ? p[1] : p[0];
        }
        s->pcm_pos += k;
        s->remaining -= (uint32_t)k;
        done += k;
    }
    return done;
}

static void adpcm_decode_block(vn_source *s, int32_t b)
{
    size_t chan_bytes = 4 + ADPCM_BLOCK / 2;
    const uint8_t *blk = s->data + s->body + (size_t)b * chan_bytes * s->channels;
    for (int c = 0; c < s->channels; c++) {
        const uint8_t *p = blk + chan_bytes * c;
        int pred = (int16_t)vn_le16(p), index = p[2];
        if (index > 88) index = 88;
        for (int n = 0; n < ADPCM_BLOCK; n++) {
            int code = (n & 1) ? p[4 + n / 2] >> 4 : p[4 + n / 2] & 15;
            int step = ima_steps[index];
            int delta = step >> 3;
            if (code & 4) delta += step;
            if (code & 2) delta += step >> 1;
            if (code & 1) delta += step >> 2;
            pred = (code & 8) ? pred - delta : pred + delta;
            if (pred < -32768) pred = -32768;
            if (pred > 32767) pred = 32767;
            index += ima_adj[code & 7];
            if (index < 0) index = 0;
            if (index > 88) index = 88;
            s->block[n * s->channels + c] = (int16_t)pred;
        }
    }
    s->block_index = b;
}

static void sample_at(vn_source *s, uint32_t i, int *l, int *r)
{
    const int16_t *p;
    if (s->kind == SRC_PCM) {
        p = (const int16_t *)(const void *)(s->data + s->body) + (size_t)i * s->channels;
        int a = (int16_t)vn_le16((const uint8_t *)p), b = s->channels == 2 ? (int16_t)vn_le16((const uint8_t *)(p + 1)) : a;
        *l = a;
        *r = b;
        return;
    }
    int32_t b = (int32_t)(i / ADPCM_BLOCK);
    if (b != s->block_index) adpcm_decode_block(s, b);
    p = s->block + (i % ADPCM_BLOCK) * s->channels;
    *l = p[0];
    *r = s->channels == 2 ? p[1] : p[0];
}

static int effect_read(vn_source *s, int16_t *out, int frames)
{
    int done = 0;
    for (; done < frames; done++) {
        uint64_t num = s->out_index * s->rate;
        uint32_t i = (uint32_t)(num / MIX_RATE);
        if (i >= s->frames) break;
        uint32_t frac = (uint32_t)(num % MIX_RATE);
        int l0, r0;
        sample_at(s, i, &l0, &r0);
        if (frac && i + 1 < s->frames) {
            int l1, r1;
            sample_at(s, i + 1, &l1, &r1);
            l0 += (int)(((int64_t)(l1 - l0) * frac) / MIX_RATE);
            r0 += (int)(((int64_t)(r1 - r0) * frac) / MIX_RATE);
        }
        out[2 * done] = (int16_t)l0;
        out[2 * done + 1] = (int16_t)r0;
        s->out_index++;
    }
    return done;
}

int source_read(vn_source *s, int16_t *out, int frames)
{
    return s->kind == SRC_MP3 ? mp3_read(s, out, frames) : effect_read(s, out, frames);
}

/* ---- mixer ------------------------------------------------------------------- */

enum { CMD_PLAY, CMD_STOP, CMD_VOLUME, CMD_MIXERS };
#define RING_SIZE 64

typedef struct cmd {
    uint8_t type, ch;
    bool loop, queue;
    float a, b, c;
    int n;
    vn_source *src[MIX_PLAYLIST_MAX];
} cmd;

typedef struct ramp {
    float value, step, target;
} ramp;

typedef struct chan {
    uint8_t mixer;
    vn_source *list[MIX_PLAYLIST_MAX];
    int n, idx;
    bool loop, playing;
    float rec_volume;
    ramp gain;                 /* fade in/out of the current track */
    ramp volume;               /* renpy.music.set_volume */
    vn_source *fading;
    float fading_volume;
    ramp fading_gain;
} chan;

struct vn_mixer {
    int nch;
    chan ch[MIX_MAX_CHANNELS];
    float mixer_vol[3];
    cmd ring[RING_SIZE];
    ring_index head, tail;     /* head: written by the engine, tail: by the mixer */
    uint32_t dropped;
};

vn_mixer *mixer_new(int nchannels, const uint8_t *mixer_of)
{
    vn_mixer *m = vn_calloc(1, sizeof *m);
    m->nch = nchannels > MIX_MAX_CHANNELS ? MIX_MAX_CHANNELS : nchannels;
    for (int i = 0; i < m->nch; i++) {
        m->ch[i].mixer = mixer_of[i] <= MIXER_VOICE ? mixer_of[i] : MIXER_SFX;
        m->ch[i].volume.value = m->ch[i].volume.target = 1.0f;
        m->ch[i].gain.value = m->ch[i].gain.target = 1.0f;
    }
    m->mixer_vol[0] = m->mixer_vol[1] = m->mixer_vol[2] = 1.0f;
    return m;
}

static void chan_clear_list(chan *c)
{
    for (int i = 0; i < c->n; i++) source_free(c->list[i]);
    c->n = c->idx = 0;
    c->playing = false;
}

void mixer_free(vn_mixer *m)
{
    if (!m) return;
    uint32_t t = RING_LOAD(m->tail), h = RING_LOAD(m->head);
    for (; t != h; t++)
        for (int i = 0; i < m->ring[t % RING_SIZE].n; i++) source_free(m->ring[t % RING_SIZE].src[i]);
    for (int i = 0; i < m->nch; i++) {
        chan_clear_list(&m->ch[i]);
        source_free(m->ch[i].fading);
    }
    free(m);
}

static void push(vn_mixer *m, const cmd *c)
{
    uint32_t h = RING_LOAD(m->head), t = RING_LOAD(m->tail);
    if (h - t >= RING_SIZE) {
        m->dropped++;
        for (int i = 0; i < c->n; i++) source_free(c->src[i]);
        return;
    }
    m->ring[h % RING_SIZE] = *c;
    RING_STORE(m->head, h + 1);
}

void mixer_play(vn_mixer *m, int ch, vn_source **sources, int n, bool loop, bool queue, float fadein, float fadeout,
                float volume)
{
    cmd c = {0};
    c.type = CMD_PLAY;
    c.ch = (uint8_t)ch;
    c.loop = loop;
    c.queue = queue;
    c.a = fadein;
    c.b = fadeout;
    c.c = volume;
    for (int i = 0; i < n; i++) {
        if (c.n < MIX_PLAYLIST_MAX)
            c.src[c.n++] = sources[i];
        else
            source_free(sources[i]);
    }
    if (ch < 0 || ch >= m->nch) {
        for (int i = 0; i < c.n; i++) source_free(c.src[i]);
        return;
    }
    push(m, &c);
}

void mixer_stop(vn_mixer *m, int ch, float fadeout)
{
    cmd c = {0};
    c.type = CMD_STOP;
    c.ch = (uint8_t)ch;
    c.b = fadeout;
    push(m, &c);
}

void mixer_channel_volume(vn_mixer *m, int ch, float volume, float delay)
{
    cmd c = {0};
    c.type = CMD_VOLUME;
    c.ch = (uint8_t)ch;
    c.a = volume;
    c.b = delay;
    push(m, &c);
}

void mixer_volumes(vn_mixer *m, float music, float sfx, float voice)
{
    cmd c = {0};
    c.type = CMD_MIXERS;
    c.a = music;
    c.b = sfx;
    c.c = voice;
    push(m, &c);
}

uint32_t mixer_dropped(const vn_mixer *m)
{
    return m->dropped;
}

static void ramp_to(ramp *r, float target, float seconds)
{
    r->target = target;
    if (seconds <= 0.0f) {
        r->value = target;
        r->step = 0;
    } else {
        r->step = (target - r->value) / (seconds * MIX_RATE);
    }
}

static void ramp_tick(ramp *r, int frames)
{
    if (r->step == 0) return;
    r->value += r->step * (float)frames;
    if ((r->step > 0 && r->value >= r->target) || (r->step < 0 && r->value <= r->target)) {
        r->value = r->target;
        r->step = 0;
    }
}

/* Moves the playing track to the fade-out slot (or drops it for an instant stop). */
static void fade_out_current(chan *c, float fadeout)
{
    source_free(c->fading);
    c->fading = NULL;
    if (c->playing && fadeout > 0.0f && c->idx < c->n) {
        c->fading = c->list[c->idx];
        c->list[c->idx] = NULL;
        c->fading_volume = c->rec_volume;
        c->fading_gain = c->gain;
        ramp_to(&c->fading_gain, 0.0f, fadeout * c->fading_gain.value);
    }
    chan_clear_list(c);
}

static void apply(vn_mixer *m, cmd *c)
{
    if (c->type == CMD_MIXERS) {
        m->mixer_vol[0] = c->a;
        m->mixer_vol[1] = c->b;
        m->mixer_vol[2] = c->c;
        return;
    }
    chan *ch = &m->ch[c->ch];
    switch (c->type) {
    case CMD_PLAY:
        if (c->queue && ch->playing) {
            for (int i = 0; i < c->n; i++) {
                if (ch->n < MIX_PLAYLIST_MAX)
                    ch->list[ch->n++] = c->src[i];
                else
                    source_free(c->src[i]);
            }
            ch->loop = c->loop;
            return;
        }
        fade_out_current(ch, c->b);
        for (int i = 0; i < c->n; i++) ch->list[i] = c->src[i];
        ch->n = c->n;
        ch->idx = 0;
        ch->loop = c->loop;
        ch->playing = c->n > 0;
        ch->rec_volume = c->c;
        ch->gain.value = c->a > 0.0f ? 0.0f : 1.0f;
        ramp_to(&ch->gain, 1.0f, c->a);
        break;
    case CMD_STOP:
        fade_out_current(ch, c->b);
        break;
    case CMD_VOLUME:
        ramp_to(&ch->volume, c->a, c->b);
        break;
    }
}

#define CHUNK 512

static void mix_source(int32_t *acc, vn_source *s, int frames, float gain_start, float gain_step, int *got)
{
    int16_t buf[CHUNK * 2];
    int done = 0;
    while (done < frames) {
        int want = frames - done > CHUNK ? CHUNK : frames - done;
        int n = source_read(s, buf, want);
        for (int i = 0; i < n; i++) {
            float g = gain_start + gain_step * (float)(done + i);
            acc[2 * (done + i)] += (int32_t)(buf[2 * i] * g);
            acc[2 * (done + i) + 1] += (int32_t)(buf[2 * i + 1] * g);
        }
        done += n;
        if (n < want) break;
    }
    *got = done;
}

void mixer_render(vn_mixer *m, int16_t *out, int frames)
{
    uint32_t t = RING_LOAD(m->tail), h = RING_LOAD(m->head);
    for (; t != h; t++) apply(m, &m->ring[t % RING_SIZE]);
    RING_STORE(m->tail, t);

    int32_t acc[CHUNK * 2];
    for (int off = 0; off < frames; off += CHUNK) {
        int n = frames - off > CHUNK ? CHUNK : frames - off;
        memset(acc, 0, sizeof(int32_t) * 2 * (size_t)n);
        for (int i = 0; i < m->nch; i++) {
            chan *c = &m->ch[i];
            float mv = m->mixer_vol[c->mixer];
            if (c->fading) {
                float g0 = c->fading_gain.value * c->fading_volume * c->volume.value * mv;
                ramp_tick(&c->fading_gain, n);
                float g1 = c->fading_gain.value * c->fading_volume * c->volume.value * mv;
                int got;
                mix_source(acc, c->fading, n, g0, (g1 - g0) / (float)n, &got);
                if (got < n || c->fading_gain.value <= 0.0f) {
                    source_free(c->fading);
                    c->fading = NULL;
                }
            }
            int pos = 0, empty_runs = 0;
            while (c->playing && pos < n) {
                float g0 = c->gain.value * c->rec_volume * c->volume.value * mv;
                ramp_tick(&c->gain, n - pos);
                ramp_tick(&c->volume, n - pos);
                float g1 = c->gain.value * c->rec_volume * c->volume.value * mv;
                int got;
                mix_source(acc + 2 * pos, c->list[c->idx], n - pos, g0, (g1 - g0) / (float)(n - pos), &got);
                pos += got;
                empty_runs = got ? 0 : empty_runs + 1;
                if (empty_runs > c->n) {
                    chan_clear_list(c);   /* every track in the playlist is empty */
                    break;
                }
                if (pos < n) {
                    /* Track ended: next in the playlist, or loop back to the first. */
                    if (c->idx + 1 < c->n) {
                        c->idx++;
                        source_rewind(c->list[c->idx]);
                    } else if (c->loop) {
                        c->idx = 0;
                        source_rewind(c->list[0]);
                    } else {
                        chan_clear_list(c);
                    }
                }
            }
            if (!c->playing) ramp_tick(&c->volume, n);
        }
        for (int k = 0; k < 2 * n; k++) {
            int32_t v = acc[k];
            out[2 * off + k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
}
