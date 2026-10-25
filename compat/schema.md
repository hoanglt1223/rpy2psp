# Compatibility data

One TOML file per game in `compat/`. The format is the one AnyGBA, AnySWF and AnyPSP use (`title`, `kind`, `source`, `stage`, `evidence`, `blockers`, `updated`), so the shared progress-page generator reads it. The fields that differ for Ren'Py games are `commit`, `game` and `languages`. `tests/test_compat.py` checks every file.

```toml
title = "The Question"
kind = "homebrew"                 # homebrew (free game) | commercial
source = "https://github.com/renpy/renpy"
commit = "860f394..."             # the exact revision that was tested
game = "the_question"             # folder inside the checkout that holds game/
license = "MIT"
languages = ["english", "japanese"]   # translations that were checked (--language)
stage = "playable"
evidence = "..."
blockers = []                     # what keeps it from the next stage, one line each
updated = 2026-10-25
```

## Stages

| Stage | Meaning for a converted visual novel |
|---|---|
| `fails` | `rpy2psp build` reports unsupported constructs, so no EBOOT is made |
| `boots` | Builds; the engine shows the title screen but stops with an error before the story ends |
| `menus` | Title and game menus work; the story does not play through |
| `in-game` | Plays start to finish in the PC engine, with every event identical to the reference interpreter (`tests/engine`) |
| `playable` | As `in-game`, and the EBOOT runs in PPSSPP with screenshots matching the PC engine (`tests/psp/ppsspp_run.py`) |
| `perfect` | As `playable`, and played start to finish on a PSP-3000, including a save and a load |

A game moves up a stage only with the evidence recorded in `evidence`. No game data is stored here: games are fetched from `source` at `commit` into a folder outside the repository.
