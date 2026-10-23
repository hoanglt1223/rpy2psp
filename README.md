# rpy2psp

Convert Ren'Py visual novels into native PSP homebrew. Scripts are compiled on PC into a compact bytecode, images become GPU-ready textures, and a small native engine plays them on a PSP with custom firmware. No Python runs on the PSP.

## Status

Early. The script compiler works. The asset pipeline and the PSP engine are not written yet.

- Scope: dialogue-driven visual novels (labels, menus, jumps, images, transitions, music, simple variables). Games that rely on custom Python code or Python-driven screens are reported as unsupported, with file and line. Details: [docs/dev/census.md](docs/dev/census.md).
- Coverage: the three target games (The Question, Zero Echoes, Arcance) compile fully. Overall, 5 of the 15 games in the census corpus compile.
- Bytecode format: [docs/dev/bytecode.md](docs/dev/bytecode.md). Prior art: [docs/dev/prior-art.md](docs/dev/prior-art.md).

Roadmap: [anyport plans](https://github.com/hoanglt1223/anyport/tree/main/plans/261023-1558-renpy-to-psp).

## Usage

Python 3.11 or later.

```sh
pip install -r requirements.txt
python -m rpy2psp compile path/to/game -o out/            # script.vnb + manifest.json, or errors with file:line
python -m rpy2psp compile path/to/game -o out/ --language japanese
python -m rpy2psp disasm out/script.vnb
python -m rpy2psp coverage --manifest tools/census/games.json --games-dir <corpus>
```

The game folder is the one that contains `game/`, or `game/` itself. The compiler reads `.rpy` sources. When only compiled scripts are available, it reads `.rpyc` files and `.rpa` archives. A restricted decoder handles them, and game code is never executed.

## Tests

```sh
python -m unittest discover -s tests -t .
```

The tests use small self-written scripts. `tests/compiler/test_target_games.py` also checks the three target games when they are present in `%TEMP%/rpy2psp-census/full`, or in the folder named by `RPY2PSP_TARGETS`. It is skipped otherwise.

## Legal

Tool and engine only. No game scripts or assets are included. Convert only games you have the right to use.

## License

MIT
