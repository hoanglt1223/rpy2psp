# Using rpy2psp

rpy2psp turns a Ren'Py visual novel into a PSP homebrew game. It runs on a PC (Windows or Linux), converts the game's scripts, images, sounds and fonts, and writes a folder that you copy to the PSP's Memory Stick. The PSP needs custom firmware (for example ARK-4) to run homebrew.

It handles dialogue-driven games: dialogue, menus, jumps, images, transitions, music and simple variables. Games that depend on their own Python code or custom screens are refused with the file and line of each unsupported construct. [SUPPORTED-FEATURES.md](SUPPORTED-FEATURES.md) lists what works.

Convert only games you have the right to use. rpy2psp ships no game content.

## Install

**Release package (no Python needed).** Download `rpy2psp-<version>-windows-x64.zip` or `rpy2psp-<version>-linux-x64.tar.gz` from the GitHub releases and unpack it. It contains the `rpy2psp` program and the PSP engine (`vnpsp/EBOOT.PBP`).

**From source.** Python 3.11 or later:

```sh
pip install -r requirements.txt
sh scripts/psp-build.sh          # the PSP engine, out/psp/vnpsp/EBOOT.PBP (needs Docker)
python -m rpy2psp --help
```

`rpy2psp build` finds the engine next to the program (`vnpsp/EBOOT.PBP`), in `out/psp/vnpsp/` of a source checkout, or where `RPY2PSP_ENGINE` points. `--engine <EBOOT.PBP>` overrides all of them. The PSP engine is also published on its own as `vnpsp-<version>-psp.zip`.

## Convert a game

```sh
rpy2psp build <game folder> -o <output>/MyNovel
```

`<game folder>` is the folder that contains `game/` (or `game/` itself). The output folder gets:

| File | What it is |
|---|---|
| `EBOOT.PBP` | The engine, with the game's title, icon (ICON0) and background (PIC1) for the PSP menu |
| `script.vnb` | The compiled story |
| `data.pak` | Images, fonts, music and sounds in PSP formats |
| `manifest.json` | What was converted (not needed on the PSP) |

Copy the output folder to `ms0:/PSP/GAME/` on the Memory Stick, so that you have `PSP/GAME/MyNovel/EBOOT.PBP`. Start it from Game > Memory Stick.

The output folder's name matters: saves go to `ms0:/PSP/SAVEDATA/<folder name>/` (letters, digits, `_` and `-` only). Keep the name when you update a game, or the saves stay behind in the old folder.

### Options

| Option | Effect |
|---|---|
| `--language <name>` | Build a translation from `game/tl/<name>/`, for example `--language japanese` |
| `--font-dir <dir>` | Extra folder to search for fonts (repeatable) |
| `--fallback-font <file>` | Font for characters the game's own fonts lack |
| `--font-size <px>` | Text size on the PSP (default 14) |
| `--title <text>` | Title in the PSP menu (default: the game's `config.name`) |
| `--icon <image>` | ICON0 image (default: `config.window_icon`, else `gui/window_icon.png`) |
| `--background <image>` | PIC1 image (default: `gui.main_menu_background`, else `gui/main_menu.png`) |
| `--pcm-limit <s>` | Sound effects up to this length stay uncompressed PCM (default 1.0) |
| `--engine <EBOOT.PBP>` | The vnpsp engine to wrap |

**Fonts.** Ren'Py games often use fonts that come with the Ren'Py SDK rather than with the game: DejaVuSans (`renpy/common` in the SDK) and, for Chinese, Japanese and Korean, SourceHanSansLite (`sdk-fonts`). When the build says a font is missing, pass those SDK folders with `--font-dir`. Only the characters the game uses are converted.

**Movies** need ffmpeg on the PATH.

**Compiled-only games.** When a game ships `.rpyc` files and `.rpa` archives instead of `.rpy` sources, rpy2psp reads those. It decodes them with a restricted reader and never runs the game's code.

### When the build fails

- `error: game/script.rpy:120: ... [feature]` lines name each construct outside the supported subset. Nothing is written. A game with a handful of errors can sometimes be fixed by editing those lines in a copy of the game.
- "the vnpsp engine EBOOT was not found": pass `--engine`, or use a release package, which includes it.
- "font ... not found": add `--font-dir`, see Fonts above.

## Playing on the PSP

| Button | In the story | In menus |
|---|---|---|
| X | Advance; finish the typing text | Confirm |
| O | Hide the text box | Back |
| D-pad, analog nub | Move in a choice menu | Move |
| L | Roll back one step | Previous save page |
| R (hold) | Skip | Next save page |
| Square | Auto-forward on or off | |
| Triangle | History | |
| START | Game menu: save, load, quick save and load, history, settings | |
| SELECT | Quick save | |

There are 30 save slots with thumbnails, a quick save and an auto save. Saves made with a different build of the same game are refused with a message rather than misread, so finish or restart a game before converting a new version. Settings (text speed, auto-forward delay, volumes) are kept in the save folder.

## Other commands

The build runs these steps; they are available on their own for debugging:

```sh
rpy2psp compile <game> -o <dir> [--language L]      # script.vnb + manifest.json, or the list of errors
rpy2psp pack <game> -o <dir> [--font-dir D]          # data.pak from manifest.json
rpy2psp verify <game> -o <dir> [--font-dir D]        # decode data.pak again and compare with the sources
rpy2psp disasm <dir>/script.vnb                      # readable listing of the compiled story
rpy2psp coverage --manifest tools/census/games.json --games-dir <dir>   # error summary over many games
```

On a PC, the engine also plays a converted folder in a window: `vnpsp <dir>` (built from source, see [docs/dev/engine.md](../dev/engine.md)).
