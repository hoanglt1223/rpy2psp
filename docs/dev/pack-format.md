# Asset pack (`data.pak`)

`rpy2psp pack <game> -o <out>` converts every asset in `<out>/manifest.json` (written by `rpy2psp compile`) into `<out>/data.pak`. `rpy2psp verify <game> -o <out>` decodes the pack back on the PC and compares each entry with its source. The source of truth is [`rpy2psp/assets/`](../../rpy2psp/assets/).

All integers are little-endian.

## Container

| Offset | Field |
|---|---|
| 0 | `char[4]` `VNPK` |
| 4 | u16 version (1), u16 flags (0) |
| 8 | u32 entry count |
| 12 | u32 index offset |
| 16 | u32 scene table offset |
| 20 | u32 scene count |
| 24 | u32 names offset |
| 28 | u32 names length |
| 64 | entry data; each entry starts on a 64-byte boundary |

- **Index** (16 bytes per entry, in storage order): u32 name offset, u8 kind, 3 bytes padding, u32 data offset, u32 data size. Kinds are 0 image, 1 music, 2 sfx, 3 voice, 4 font, 5 movie. The name is the asset key, the same key as in the `ASET` section of `script.vnb`. When two entries hold identical data, they share it.
- **Scenes** (12 bytes per scene): u32 name offset, u32 index of the first item in the scene list, u32 count. The scene list follows the scene table and is a series of u32 entry indexes. A scene is a top-level label. Entries are stored in the order scenes first use them, so a scene's assets are mostly contiguous on the Memory Stick and the engine can prefetch them when it enters the label.
- **Names:** UTF-8 strings, each ending in NUL.

Fonts use reserved keys:

- `font/text`: dialogue;
- `font/name`: speaker names;
- `font/interface`: menu choices and the UI;
- `font/face/<file>`: one per `{font=}` tag.

## Images: `TEX1`

| Field | Type |
|---|---|
| magic | `char[4]` `TEX1` |
| width, height | u16 each: size in PSP pixels |
| source width, source height | u16 each: size in game pixels |
| format | u8 GE texture format: 8 DXT1 (opaque), 10 DXT5 (any alpha) |
| tile count | u8 |
| reserved | u16 |
| padding | 4 bytes, then the tile table at offset 16 |

Each tile takes 24 bytes:

| Field | Type |
|---|---|
| x, y, w, h | u16 each: where the tile sits inside the image |
| texture width, texture height | u8 log2 each: the power-of-two values for `sceGuTexImage` |
| buffer width | u16, in pixels |
| rows | u16: rows stored, a multiple of 4 |
| reserved | u16 |
| data offset | u32, from the start of the entry, 16-byte aligned |
| data size | u32 |

**Scaling.** Images are scaled by `min(480 / source width, 272 / source height)`. Lanczos resampling is done on premultiplied alpha, so transparent edges do not darken. Tiles are at most 512x512, the GE limit. A full-screen 1280x720 background becomes one 512x272 DXT1 tile of 69,632 bytes. As DXT5, the same tile is 139,264 bytes.

**Padding.** The texture width is padded to a power of two by repeating the edge pixels. Padded pixels are fully transparent in DXT5. Only `rows` rows are stored, while the texture height register is the next power of two. The engine must keep V coordinates within `h / 2^tex_h_log2`.

**DXT block layout** (from PPSSPP `GPU/Common/TextureDecoder.h`):

- The GE reads DXT as a row-major grid of 4x4 blocks and has no swizzled DXT mode, so DXT data is never swizzled.
- A DXT1 block is `u8 lines[4]`, then `u16 color0`, then `u16 color1`. This is the reverse of DDS order.
- A DXT5 block is the DXT1-style colour block, then `u32` and `u16` holding the 48 bits of alpha indexes, then `u8 alpha0`, then `u8 alpha1`.

**RGB565 expansion.** The GE expands RGB565 by shifting, so 31 becomes 248, with no low-bit replication. The encoder pre-scales colours by 255/248 so the endpoints it picks land where the GE will output them.

## Fonts: `FNT1`

The header is 20 bytes:

| Field | Type |
|---|---|
| magic | `char[4]` `FNT1` |
| size | u16, in pixels |
| ascent | i16 |
| descent | i16, stored negative |
| line height | i16 |
| format | u8: 4 T4, 5 T8 |
| page count | u8 |
| glyph count | u16 |
| outline | u8, in pixels |
| reserved | u8 + u16 |

Then come the pages, then the glyphs:

- **Pages** (12 bytes each): u16 width (512), u16 rows, u32 offset, u32 size. Page data is swizzled for the GE: blocks of 16 bytes x 8 rows. Pages are 512 pixels wide and `rows` tall, where `rows` is a multiple of 8.
- **Glyphs** (20 bytes each, sorted by code point): u32 code point, u16 x, u16 y, u16 w, u16 h, i16 bearing x, i16 bearing y (from the baseline up to the bitmap top), u16 advance in 1/64 pixel, u8 page, u8 reserved.

Pixel formats:

- **T4** (GU_PSM_T4): each pixel is 4-bit coverage, low nibble first. The engine loads a 16-entry CLUT that ramps the text colour's alpha from 0 to 255.
- **T8** (GU_PSM_T8): used when the game sets outlines. The low nibble is the fill, the high nibble is the outline (the fill dilated by `outline` pixels). The engine draws each glyph twice with two 256-entry CLUTs: first one keyed on the high nibble with the outline colour, then one keyed on the low nibble with the text colour.

**Character set.** It is every character in the story's display text after build-time translation and NFC normalisation, plus printable ASCII (needed for `renpy.input` and interpolated numbers).

**Font lookup.** Fonts are taken from the game first (`tl/<language>/`, the game root, `fonts/`). After that, the `--font-dir` folders are searched; the files that Ren'Py ships with the SDK, such as `DejaVuSans`, are not part of the game. A character the font lacks is a build error unless `--fallback-font` supplies it. Sizes are set by `--font-size` (default 14 px), because vnpsp draws its own text box instead of the game's.

## Music

Music entries are a 16-byte `MUS1` header followed by an MP3 stream: MPEG-1 Layer III, 44.1 kHz, 128 kbps CBR, encoded with LAME. The source channel count is kept.

| Field | Type |
|---|---|
| magic | `char[4]` `MUS1` |
| frames | u32: length of the source in sample frames at 44.1 kHz |
| delay | u16: decoded frames to drop before the first source frame (1105: LAME's 576 plus the decoder's 529) |
| channels | u8 |
| reserved | u8 |
| sample rate | u32 (44100) |

The engine decodes the stream with minimp3, drops `delay` frames, plays exactly `frames` frames, and for a looping track starts again from the first MP3 frame. Loops are therefore gapless.

## Sound effects and voice: `SND1`

The header is 16 bytes:

| Field | Type |
|---|---|
| magic | `char[4]` `SND1` |
| sample rate | u32: the source rate if it is 11025, 22050, 44100, or 48000, otherwise resampled to 44100 |
| channels | u8 |
| codec | u8: 0 PCM16, 1 IMA ADPCM |
| frames per block | u16 (1024) |
| frame count | u32 |

**Codec choice.** A clip lasting `--pcm-limit` seconds or less (default 1 s) is stored as PCM16, interleaved. Anything longer is IMA ADPCM, 4:1.

**ADPCM blocks.** Each block covers 1024 frames. For each channel in turn it holds `i16 predictor, u8 step index, u8 0`, then 512 bytes of 4-bit codes, low nibble first. The predictor and step index are the decoder state before the block's first sample, so every block decodes on its own. The last block is padded with silence.

## Movies: `MOV1` (optional)

| Field | Type |
|---|---|
| magic | `char[4]` `MOV1` |
| H.264 size | u32 |
| MP3 size | u32 |
| width, height | u16 each (480, 272) |
| frame rate | f32 (29.97) |

The H.264 stream follows the header, then the MP3 audio.

- **Video:** H.264 Baseline, level 3.0, Annex B elementary stream, 480x272, letterboxed.
- **Audio:** the music settings.

Conversion needs ffmpeg on PATH. None of the target games uses movies. Whether the stream plays on the hardware decoder is still to be shown in phase 04.

## Verification

`rpy2psp verify` decodes every entry and compares it with a fresh conversion of the source:

| Kind | Check | Fails when |
|---|---|---|
| Images | Independent DXT decoder with the GE's semantics; PSNR of the displayed (premultiplied) colour and alpha error, against the scaled source | PSNR is below 20 dB or mean alpha error is above 4 |
| Fonts | Every glyph is unswizzled, unpacked, and compared with a fresh render | Any glyph differs |
| Music | The MP3 header is parsed, then the stream is decoded with libsndfile, aligned by cross-correlation (encoder delay), and the SNR and duration measured | The header is not 1/3/128/44100, the duration differs by more than 0.1 s, SNR is below 10 dB, the `MUS1` length or channel count differs from the source, or a measurable lag differs from the stored delay |
| Effects | Decoded and compared with the resampled source | PCM is not identical, or ADPCM SNR is below 20 dB |

The unit tests also check the GE decoder against Pillow's standard DDS decoder. Alpha must be identical, and RGB may differ only by the 0-7 expansion offset.
