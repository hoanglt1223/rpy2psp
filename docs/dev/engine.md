# vnpsp: the engine

vnpsp plays a converted game: `script.vnb` ([bytecode.md](bytecode.md)) and `data.pak` ([pack-format.md](pack-format.md)). It is C11, and the same code builds for the PC and the PSP. On the PC it is the development and test target. It runs headless from a command script, with no window and no audio device.

## Layout

| Path | Contents |
|---|---|
| `vnpsp/src/vnb.c`, `vm.c`, `value.c` | Script loader, bytecode interpreter, and Python-semantics values |
| `vnpsp/src/scene.c`, `atl.c`, `transitions.c` | Shown images, transforms, `with` transitions |
| `vnpsp/src/text.c`, `menu.c` | Text layout and tags, choice menus, screens, on-screen keyboard |
| `vnpsp/src/save.c`, `rollback.c`, `settings.c` | Checkpoints, save files, persistent data, rollback, preferences |
| `vnpsp/src/assets.c`, `audio.c` | Pack reader and caches, decoders and mixer |
| `vnpsp/src/engine.c` | Story state machine and the instructions that touch the screen or sound |
| `vnpsp/src/gfx_soft.c`, `gfx_gu.c` | Renderers: software on the PC, GE on the PSP |
| `vnpsp/src/main.c`, `driver.c` | Entry point; the headless script driver (PC) |
| `third_party/psp-platform/` | AnySWF's platform layer, vendored (see its `VENDORED.md`) |
| `third_party/minimp3/` | MP3 decoder (CC0) |

The core (`vncore`) does not depend on the platform layer, except through `gfx_gu.c` on the PSP. `main.c` owns the window or display, the audio thread, and frame pacing.

## Building and running

On the PC, with CMake and MSVC (SDL3 is fetched at configure time; on Linux use `-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang` and `out/linux`):

```sh
cmake -S . -B out/msvc -G "Visual Studio 17 2022" -A x64 -DPython3_EXECUTABLE=<python with requirements.txt>
cmake --build out/msvc --config Release
ctest --test-dir out/msvc -C Release --output-on-failure
```

To play a game, convert it first:

```sh
python -m rpy2psp compile <game> -o out/game && python -m rpy2psp pack <game> -o out/game --font-dir <SDK fonts>
out/msvc/vnpsp/Release/vnpsp.exe out/game                  # windowed player (keyboard or gamepad)
out/msvc/vnpsp/Release/vnpsp.exe out/game --headless --frames 240                 # hidden window, dummy audio
out/msvc/vnpsp/Release/vnpsp.exe out/game --play script.txt --trace trace.txt     # scripted, no window at all
```

For the PSP, `scripts/psp-build.sh` builds `out/psp/vnpsp/EBOOT.PBP` in the pinned pspdev Docker image. `rpy2psp build <game> -o <folder>` wraps it with the game's title and images next to `script.vnb` and `data.pak`; copy that folder to `ms0:/PSP/GAME/`. Saves go to `ms0:/PSP/SAVEDATA/<folder>/`, the folder name reduced to letters, digits, `_` and `-`.

### Testing on a PSP-3000 (ARK-4)

1. `sh scripts/psp-build.sh`, then `python -m rpy2psp build <game> -o <out>/<Folder> --font-dir <fonts>`.
2. Copy `<out>/<Folder>` over USB to `ms0:/PSP/GAME/<Folder>/`. Check that the XMB shows the game's title, icon and background.
3. On the PSP: Game > Memory Stick > the game. Play through the first scenes and compare with the PC `shot` PNGs.
4. Check: text and CJK glyphs render, a choice menu works, music loops without gaps, save then load from `ms0:/PSP/SAVEDATA/<Folder>/`, rollback, and no slowdown on scene changes.
5. For crash logs, build a PRX (`PSP_CMAKE_ARGS=-DBUILD_PRX=ON sh scripts/psp-build.sh`) and run it through psplink (`usbhostfs_pc` + `pspsh`) from `host0:` in a folder that also holds `script.vnb` and `data.pak`; exceptions print in `pspsh`.
6. Report the first scene that looks wrong or the crash text.

### Headless driver

`--play` runs a command script and presses buttons the way a player would. The trace has one line per story event (`say`, `menu`, `show`, `play`, and the others; the formats match `tests/engine/harness.py`), the shown text, and the results of the `hash`, `state`, `shot` and `save` commands. The command list is at the top of `vnpsp/src/driver.c`.

| Option | Effect |
|---|---|
| `--fast` | Skip typing, pauses and transitions with X |
| `--render-all` | Draw every frame, not only those a `hash` or `shot` needs |
| `--audio`, `--wav FILE` | Mix audio every frame and report a hash, or also write the mix to a WAV file. Nothing is played |
| `--texture-budget BYTES` | Size of the decoded-image cache (default 12 MiB) |

## Controls

| Button | In the story | In menus and screens |
|---|---|---|
| X | Advance; finish the typing text | Confirm |
| O | Hide the text box | Back |
| D-pad, analog nub | (in a menu) move | Move |
| L | Roll back one interaction | Previous save page |
| R (hold) | Skip | Next save page |
| Square | Auto-forward on or off | Space (keyboard) |
| Triangle | History | |
| START | Game menu | Done (keyboard) |
| SELECT | Quick save | |

On the PC, the arrow keys are the D-pad; Z, X, A and S are X, O, Square and Triangle; Q and W are L and R; Enter is START and right Shift is SELECT; Escape quits.

## How a story runs

- **The VM** runs instructions until one needs the engine (a `say`, a `menu`, `show`, `play`, and so on), and stops there before executing it. The engine checkpoints if the instruction is an interaction, then carries it out.
- **Values follow Python.** This covers integers, floats with Python's `repr`, strings indexed by character, lists, tuples, dicts, sets, `%` formatting and `format()` specs. `renpy.random` uses Python's Mersenne Twister and its seeding, so a run with seed 0 matches the reference interpreter. `upper`, `lower`, `title` and `capitalize` change ASCII letters only.
- **Checkpoints** are taken at every interaction that waits for the player: a line, a menu, a click pause, and `renpy.input`. A timed `pause` is not a checkpoint, as in Ren'Py. A checkpoint is the whole story state serialized: the VM (stack, call frames, variables, random state), shown images with their transform time, the window mode, the NVL page, and what each audio channel should be playing. Lists shared between variables stay shared after a restore.
- **Rollback** keeps the last 128 checkpoints, within 2 MiB. L restores the previous one and runs its interaction again. History entries after it are dropped.
- **Saves** are a checkpoint plus a 120x68 thumbnail, the time, a description and the history, with a CRC-32. There are 30 slots (`slot-NN.sav`), `quick.sav` and `auto.sav`. A save records the hash of `script.vnb`; a save made with a different build is refused with a message instead of being misread. Every file is written to a temporary file and then renamed.
- **Persistent variables** live in `persistent.bin`, keyed by name, so they survive a rebuild of the game. They are written when they change, at the next checkpoint, and on exit. **Preferences** (text speed, auto-forward delay, three volumes) live in `settings.bin`.

## Drawing

Positions are computed in the game's own pixels and drawn at `min(480 / w, 272 / h)` scale with a letterbox. Images are already scaled to that size by the converter. An image that is neither scaled nor rotated is drawn at its own pixel size on whole pixels, so it stays sharp.

- **Transforms.** A sprite's properties come from Ren'Py's default transform, then the image's own `Image(..., xalign=...)`, then the `at` list, then the `show` ATL block. ATL is replayed from time 0 each frame. Its state therefore depends only on elapsed time, and saves and rollback store just that time. A `repeat` with no count skips whole passes, so the cost stays flat.
- **Transitions.** The old screen is drawn once into an off-screen target, then composited with the live new scene: dissolve, fade, wipes, slides, pushes and pixellate. Move transitions interpolate each tag's position, with enter and leave transforms. `vpunch` and `hpunch` shake the layer.
- **Text.** Glyphs come from the FNT1 atlases with a CLUT per colour, two passes for outlines. Lines break at spaces, and between CJK characters except before closing punctuation (kinsoku). All the text tags listed in bytecode.md are drawn: bold is a second pass one pixel to the right, italic is a shear, and `{size}` scales relative to Ren'Py's default text size for the game's resolution. The box grows to fit the line. Side images come from `show_side_image`, or from the `side <tag> <attributes>` image of the speaking character's tag.
- **Software renderer (PC).** It decodes DXT exactly as the GE does (PPSSPP's block layout, RGB565 expanded by shifting). It samples bilinearly, clamped to the stored texels, and blends like the GE's src-alpha mode. It is deterministic, so tests compare frame hashes. The windowed PC player uploads its frame to an OpenGL texture.
- **GE renderer (PSP).** It draws into the platform layer's display list. Textures are read from RAM (16-byte aligned, written back from the data cache), and textures of 64 KiB or less are also copied into a VRAM pool with LRU eviction. The VRAM layout is: the platform's two 8888 frame buffers and depth buffer, then two 5650 transition targets (272 KiB each), then the pool (about 144 KiB).

## Assets and memory

Decoded images stay in an LRU cache, 12 MiB by default. A texture drawn in the current frame is never evicted, so a scene larger than the budget still draws, at the cost of reloading. When the story jumps or calls into a label that is a scene in the pack, that scene's images are read in one go. The trace reports the bytes as `prefetch`. In the target games the largest scene is 348 KB, which is about 0.2 s from a Memory Stick at 1.5 to 2 MB/s. Fonts are loaded once at start.

## Audio

Each script channel has a playlist, looping, fade in and out, and a volume (`renpy.music.set_volume`). Channels feed three mixers: music, sound, and voice. The mixer runs on the audio thread, and the engine sends it commands through a single-producer, single-consumer ring.

Music is decoded with minimp3 on both platforms. `sceMp3` is not used, so the PC tests exercise the same decoder the PSP runs. The decoder drops the MUS1 delay, plays exactly the stored length, then restarts at the first frame, so loops are gapless. PCM and IMA ADPCM effects are resampled to 44.1 kHz linearly. A voice line stops at the next interaction unless `voice sustain` is used. After a load or a rollback, looping tracks restart if they changed; one-shot sounds are not replayed.

## Not supported yet

- **Movies.** `renpy.movie_cutscene` is skipped with a log line. No target game uses it.
- **Window transitions.** `window show dissolve` and `nvl show dissolve` switch at once, without the transition.
- **Keyboard.** The `renpy.input` keyboard offers printable ASCII, the characters the converter guarantees in every font.
- **Rollback and choices.** Rolling back past a menu and choosing again is allowed; there is no fixed rollback.

## Verification status

**Verified on the PC (headless).** These are covered by `ctest` and `python -m unittest discover -s tests -t .`:

- Every menu path of the self-written test game, and every path or a sample of them for the three target games, produces the same event trace as the reference interpreter (`tests/compiler/refvm.py`). This includes typed passwords and real timing.
- Text values match Python for about 100 expressions and format specs.
- Saving and loading restores the same state and the same frame.
- Rolling back 55 checkpoints in Arcance restores the earlier state, and playing forward again reaches the same state.
- The same script gives identical traces, frames and audio hashes on every run.
- A tiny texture cache draws the same frames as a large one.
- Music loops without a gap, and the MP3 trim gives the exact length and phase.
- Damaged saves are refused; persistent data survives a restart; no glyph is missing in any target, Japanese included.

**Verified in PPSSPP** (PPSSPPHeadless, the EBOOT from `scripts/psp-build.sh` wrapped by `rpy2psp build`):

- `tests/psp/ppsspp_run.py` plays a timeline of button presses in PPSSPP and in the PC engine and compares the screenshots. For The Question (`tests/psp/the-question.timeline`: title, dialogue, a menu choice, save, more lines, load, then the rest of that ending and back to the title) all 17 shots match at 31 to 51 dB PSNR; the two lowest differ only in the save's timestamp, which the deterministic PC run leaves blank. The opening of the Japanese build, Zero Echoes (Chinese) and Arcance (`tests/psp/first-lines.timeline`) match at 36 to 52 dB. This covers the GE renderer with DXT and CLUT glyph textures, the VRAM pool, transitions, save thumbnails and CJK text.
- Saves are written to `ms0:/PSP/SAVEDATA/<EBOOT folder>/` (`slot-01.sav`, `persistent.bin`), and loading slot 1 returns to the saved line.
- `tests/psp/music_loop_check.py` records a self-made 2 s loop through PPSSPP's audio dump: the PSP mix equals the PC engine's within 1 LSB over five loops, so the audio thread, minimp3 and the gapless trim behave as on the PC.
- Timing from the engine's log (emulated, so indicative only): start-up 54 to 70 ms, the largest first scene (Arcance, 348 KB) loads in 36 ms, 59.9 fps with 0.2 to 0.5 ms of CPU per frame, the mixer with MP3 decoding takes 4.4 to 5.3% of real time, and writing or loading a save stalls one frame for about 0.3 s.

**Not verified.** The PSP-3000 itself: real Memory Stick read times (PPSSPP emulates I/O timing), minimp3's real cost at 333 MHz, memory headroom, and the XMB showing the title, ICON0 and PIC1.

### Timing log

On the PSP and in the PC player, the engine logs `vnpsp: started in N ms`, `vnpsp: scene <label>: <bytes> bytes in N ms` when a scene's images are prefetched, and every 5 seconds `vnpsp: perf: <fps>, frame cpu <avg> / <max> us, audio mix <share> of real time`. On the PSP these lines go to stdout, which psplink's `pspsh` and PPSSPP show. The scripted driver (`--play`) passes no clock, so its traces stay deterministic.

### Testing in PPSSPP

```sh
python -m rpy2psp build <game> -o <scratch>/TheQuestion --font-dir <fonts>
python tests/psp/ppsspp_run.py <scratch>/TheQuestion tests/psp/the-question.timeline -o <scratch>/run --vnpsp <PC vnpsp>
python tests/psp/music_loop_check.py -o <scratch>/loop --vnpsp <PC vnpsp>
```

Both scripts run `PPSSPPHeadless` from the `ppsspp-headless:local` Docker image by default (`--ppsspp` changes the command). `ppsspp_run.py` writes `report.json` with the PSNR of each shot, the timing lines and the save files, and exits with 1 when a shot differs, a scene takes a second or more, or the engine never starts.
