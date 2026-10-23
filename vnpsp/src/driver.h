/* Headless scripted runs on PC (see driver.c for the command language). */
#ifndef VN_DRIVER_H
#define VN_DRIVER_H

#include <stdbool.h>
#include <stddef.h>

#include "fs.h"

typedef struct driver_opts {
    long max_frames;
    bool render_all;      /* draw every frame instead of only when a hash or shot needs it */
    bool fast;            /* skip text typing, pauses and transitions with X */
    bool audio;           /* mix audio every frame and report its hash */
    const char *wav_path; /* also write the mix to a WAV file (never played) */
    size_t texture_budget;
} driver_opts;

int driver_main(const char *game, const char *save_dir, const char *script_path, const char *trace_path,
                const driver_opts *o);

#endif
