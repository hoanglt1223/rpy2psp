# Ren'Py feature census

Counts which Ren'Py features a set of games use, and checks each game against the
supported subset in `subset.json`. Results and the subset decision are in
[`docs/dev/census.md`](../../docs/dev/census.md).

Python 3.10+ standard library only. The tool parses; it never imports Ren'Py and
never compiles or executes game code (Python fragments go through `ast.parse` only).

## Run

```sh
# 1. Fetch the corpus (blobless git clones, only *.rpy checked out) outside the repo
python -I fetch_games.py "%TEMP%/rpy2psp-census/games"

# 2. Census + coverage report
python -I census.py --manifest games.json --games-dir "%TEMP%/rpy2psp-census/games" ^
    --json "%TEMP%/rpy2psp-census/out.json" --markdown "%TEMP%/rpy2psp-census/out.md"

# Any local game folder (the folder holding the .rpy files)
python -I census.py path/to/game

# Tests (self-authored snippets)
python -I -m unittest test_census
```

## Files

| File | Role |
|---|---|
| `rpy_lexer.py` | Logical lines and indentation tree, following `renpy/lexer.py` rules |
| `analyzer.py` | Walks statements, ATL, screens, and Python fragments; emits feature keys |
| `census.py` | CLI, coverage against `subset.json`, what-if scenarios, markdown report |
| `fetch_games.py` | Reproduces the corpus from `games.json` (pinned commits) |
| `games.json` | Corpus manifest: repo, commit, game root, era, license |
| `subset.json` | `supported`, `ignored`, and cumulative `scenarios` feature patterns (fnmatch) |

## Feature keys

`stmt:*` statements, `say:*` dialogue forms, `menu:*`, `label:*`/`call:*`/`jump:*`,
`show:*`/`scene:*`/`hide:*` clauses, `at:*` transforms in `at` lists, `trans:*`
transitions (`trans:<builtin>`, `trans:ctor:<Class>`, `trans:defined:<Class>` for
names bound by `define`), `atl:*`, `audio:*`, `text:tag:*`, `char:*` Character
arguments, `image:*`, `py:*` Python shapes (`py:assign:*`, `py:call:<dotted>`,
`py:method:<name>`, `py:stmt:<AstNode>`, `py:expr:<AstNode>`, `py:attr:<root>`),
`screen:<call|show|hide>:<default|custom-static|custom-dynamic|undefined>`,
`idiom:*` recognised build-time idioms, and informational `cfg:*`, `ui:*`, `sl:*`,
`tl:*`, `era:*`.

A game is *covered* when every key it uses matches a `supported` or `ignored` pattern.

## Rules worth knowing

- Files named `gui.rpy`, `screens.rpy`, and `options.rpy` belong to Ren'Py's
  project template and configure the default UI, which vnpsp replaces natively.
  Their features get a `ui:` prefix and are ignored for coverage.
- `config.*`, `gui.*`, `build.*`, `style.*`, `theme.*`, `layout.*` assignments and
  `style` statements are `cfg:` (ignored).
- Translations under `tl/` only contribute their language names.
- A custom screen is `custom-static` when its tree has no `$`, `python`, `for`,
  `showif`, `drag`, `draggroup`, `input`, `mousearea`, `vpgrid` lines and every call in it
  is a plain action (`Jump`, `Return`, `SetVariable`, `Show`, `Hide`, ...),
  a static displayable, or a transition.
- `idiom:auto-images` is the cookbook loop that defines an image for every file in
  a folder (`for f in os.listdir(...)` or `renpy.list_files()` plus `renpy.image`).

## Why not Ren'Py's own parser

`renpy/parser.py` imports `renpy`, whose package init pulls in the display and audio
stack, and the parser itself depends on Cython modules (`astsupport.pyx`,
`lexersupport.pyx`, `cslots.pyx`). Using it needs a built Ren'Py SDK runtime, and
the SDK's command-line `compile`/`lint` paths run the game's init code, which this
census must never do. A standalone parser was the safe option.

## Limits of the standalone parser

- Statement recognition is keyword-based. A user-defined statement (`renpy.register_statement`)
  shows up as `stmt:unknown:<word>`; a Character defined only inside a nested Python
  block can show up as `say:who-unknown`.
- Definitions are collected from `define`/`default`, top-level assignments in
  `init python`, and `transform` statements. Values built at runtime are not resolved,
  so `with some_variable` reports `trans:unknown` or `trans:variable`.
- ATL is classified by its first token; property values and custom warpers
  (`atl:expr`) are not evaluated.
- Screen language is only counted (`sl:*`) and classified static or dynamic by the
  heuristic above; `use` of another screen does not inherit that screen's verdict.
- Python 2-only syntax (Ren'Py 6/7 games) fails `ast.parse` and is reported as
  `py:unparsable` rather than analysed.
- Asset formats come from the git file listing (extensions only, no sizes).
- `.rpyc`/`.rpa`-only games are not read; phase 02 adds those readers.
