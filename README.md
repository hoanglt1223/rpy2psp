# rpy2psp

Convert Ren'Py visual novels into native PSP homebrew. Scripts are compiled on PC into a compact bytecode, images become GPU-ready textures, and a small native engine plays them on a PSP with custom firmware. No Python runs on the PSP.

## Status

The script compiler, the asset pipeline, and the engine (vnpsp) work on the PC. The three target games play start to finish there, headless, with saves, rollback and Japanese text. The PSP build of the engine is written but has not been built or run on a PSP yet. Packaging into a ready-to-copy EBOOT folder comes next.

- Scope: dialogue-driven visual novels (labels, menus, jumps, images, transitions, music, simple variables). Games that rely on custom Python code or Python-driven screens are reported as unsupported, with file and line. Details: [docs/dev/census.md](docs/dev/census.md).
- Coverage: the three target games (The Question, Zero Echoes, Arcance) compile fully. Overall, 5 of the 15 games in the census corpus compile.
- Formats: [docs/dev/bytecode.md](docs/dev/bytecode.md) (script) and [docs/dev/pack-format.md](docs/dev/pack-format.md) (DXT textures, glyph atlases, MP3, PCM and ADPCM). Prior art: [docs/dev/prior-art.md](docs/dev/prior-art.md).
- Engine: [docs/dev/engine.md](docs/dev/engine.md) (building, controls, saves, rendering, what is verified).

Roadmap: [anyport plans](https://github.com/hoanglt1223/anyport/tree/main/plans/261023-1558-renpy-to-psp).

## Usage

Python 3.11 or later.

```sh
pip install -r requirements.txt
python -m rpy2psp compile path/to/game -o out/            # script.vnb + manifest.json, or errors with file:line
python -m rpy2psp compile path/to/game -o out/ --language japanese
python -m rpy2psp pack path/to/game -o out/ --font-dir <Ren'Py SDK>/renpy/common   # data.pak
python -m rpy2psp verify path/to/game -o out/ --font-dir <Ren'Py SDK>/renpy/common # decode back and compare
python -m rpy2psp disasm out/script.vnb
python -m rpy2psp coverage --manifest tools/census/games.json --games-dir <corpus>
```

Windows builds are attached to GitHub releases (`rpy2psp.exe`, no Python needed). Ren'Py's default font (DejaVuSans) and the CJK font (SourceHanSansLite) come with the Ren'Py SDK, not with games, so pass `--font-dir` when a game relies on them. Movie conversion needs ffmpeg on PATH.

The game folder is the one that contains `game/`, or `game/` itself. The compiler reads `.rpy` sources. When only compiled scripts are available, it reads `.rpyc` files and `.rpa` archives. A restricted decoder handles them, and game code is never executed.

### Engine

The engine is C11 with CMake. On Windows (MSVC):

```sh
cmake -S . -B out/msvc -G "Visual Studio 17 2022" -A x64 && cmake --build out/msvc --config Release
out/msvc/vnpsp/Release/vnpsp.exe out/                      # play a converted game in a window
sh scripts/psp-build.sh                                    # PSP EBOOT.PBP via the pspdev Docker image
```

On the PSP, `script.vnb` and `data.pak` go next to `EBOOT.PBP`. Tagged releases attach the PSP build (`vnpsp-<tag>-psp.zip`). Controls and details: [docs/dev/engine.md](docs/dev/engine.md).

## Tests

```sh
python -m unittest discover -s tests -t .
ctest --test-dir out/msvc -C Release --output-on-failure   # engine unit tests, the test game, a platform smoke run
```

The engine tests in `tests/engine/` build a self-written test game, play it headless through the engine, and compare every event with the reference interpreter. They are skipped until the engine is built (or `VNPSP_EXE` names it).

The tests use small self-written scripts. `tests/compiler/test_target_games.py` and `tests/engine/test_target_games.py` also check the three target games when they are present in `%TEMP%/rpy2psp-census/full`, or in the folder named by `RPY2PSP_TARGETS`. They are skipped otherwise.

## Legal

Tool and engine only. No game scripts or assets are included. Convert only games you have the right to use.

## License

MIT. Third-party code: [THIRD_PARTY.md](THIRD_PARTY.md).
