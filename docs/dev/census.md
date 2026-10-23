# Ren'Py feature census and supported subset

Run on 2026-10-23 with [`tools/census`](../../tools/census/README.md) (standalone parser,
parse only, no game code executed). The corpus is pinned in
[`games.json`](../../tools/census/games.json) and the subset in
[`subset.json`](../../tools/census/subset.json). To regenerate the full tables:

```sh
python -I tools/census/fetch_games.py "%TEMP%/rpy2psp-census/games"
python -I tools/census/census.py --manifest tools/census/games.json \
    --games-dir "%TEMP%/rpy2psp-census/games" --markdown "%TEMP%/rpy2psp-census/out.md"
```

## Result

**The original phase 01 gate (10 of 15) was not met. The user then narrowed the scope to
dialogue-driven VNs (see [Compiler coverage](#compiler-coverage-phase-02)).** The core subset fully covers 5 of 15 games
(the_question, wanted_dragon, chemistry_hero, arcance, zero_echoes), not 10. Adding static
custom screens and the missing effects brings it to 7 of 15. The other 8 games need
Python functions and classes plus custom screens that run Python. That means a Python
runtime with Ren'Py's screen language, which is the non-goal in `plan.md`.

The "at least three target games" half of the gate **is** met: three permissively licensed
games are fully inside the core subset (see [Target games](#target-games)).

Cumulative what-if coverage (each row adds to the one above):

| Step | Gate corpus (15) | Jam corpus (7) |
|---|---|---|
| Core subset | **5**: the_question, wanted_dragon, chemistry_hero, arcance, zero_echoes | 1: cat_adventure |
| + static custom screens (fixed UI tree; Jump/Return/SetVariable/Show/Hide actions) | 6: + rms_dating_sim | 1 |
| + extra effects (ImageDissolve, blinds, iris, zoom, CropMove, layeredimage, ConditionSwitch, Movie sprites, matrixcolor, blur, onlayer) | 7: + marystory | 1 |
| + AOT-compiled Python functions (`def`/`if`/`for`/`return` over the safe subset) | 7 | 2: + our_home |
| + dynamic screens and Python classes (Ren'Py runtime territory) | 11 | 7 |

Compiling Python functions on their own adds no gate games, because every game that
defines functions also uses dynamic screens or classes. The 4 games still open at the last
step (ksre, personal_space, personal_space2, cross_channel) are held back only by minor
clauses (`label ... hide`, `scene ... onlayer`) and by speakers the standalone parser
could not resolve (`say:who-unknown`). With a full runtime they would effectively be
covered.

## Compiler coverage (phase 02)

The scope decision (2026-10-23) narrows rpy2psp to dialogue-driven VNs. The gate is now:
the three target games compile fully, and coverage of this corpus is reported. The phase 02
compiler measures coverage directly. A game counts as covered when `rpy2psp compile` reports
zero errors. Asset files that a sparse census checkout omits are known from the file listing,
so they are not counted as missing.

```sh
python -m rpy2psp coverage --manifest tools/census/games.json --games-dir "%TEMP%/rpy2psp-census/games"
```

| Game | Errors | Main reasons |
|---|---|---|
| the_question, arcance, zero_echoes (targets) | 0 | - |
| wanted_dragon, chemistry_hero | 0 | - |
| rms_dating_sim | 5 | static custom screen `countdown` (timed menu) |
| wonder | 6 | Python functions, `renpy.curry`, custom bestiary screen |
| marystory | 20 | `Movie` sprites, `irisout`, custom choice screen; 15 audio files absent from the repo |
| enkindle, unsupervised | 34, 48 | Python functions, imports, and custom screens |
| learntocoderpg, personal_space, personal_space2, ksre, cross_channel | 1,300 to 33,898 | Python classes and functions, custom screens, transitions held in variables; characters created in Python cascade into `say:who-unknown` |

**Result: 5 of 15 compile with zero errors.** These are the same five games the census
predicted. The three targets are among them. Earlier sections of this page describe the
original 10-of-15 gate and the reasoning behind narrowing it.

## Corpus

Fifteen open-source games with `.rpy` sources on GitHub, downloadable without a login,
spanning Ren'Py 6 to 8. A supplementary set of seven NaNoRenO jam games tests whether
small jam games are simpler. They are not: 1 of 7 is covered, and that one has 2 lines of
dialogue. Clones are blobless and sparse (only `.rpy` checked out) under
`%TEMP%/rpy2psp-census`, never in the repo.

| Game | Era | License | `.rpy` files | Say lines | Languages (tl) | Covered |
|---|---|---|---|---|---|---|
| [the_question](https://github.com/renpy/renpy/tree/master/the_question) | 8 (8.5 sample game) | MIT (Ren'Py license; demo art under the same terms) | 52 | 71 | 12 incl. japanese, korean, schinese, tchinese | yes |
| [learntocoderpg](https://github.com/freeCodeCamp/LearnToCodeRPG) | 7.4 to 8 | BSD-3-Clause | 60 | 3,525 | english, simplified_chinese | no |
| [ksre](https://github.com/fleetingheart/ksre) (Katawa Shoujo) | 6-era content on 8 | MPL-2.0 code, assets CC BY-NC-ND 3.0 | 309 | 32,260 | 9 | no |
| [personal_space](https://github.com/qirien/personal-space) | 6 | GPL-3.0 program/text | 59 | 7,473 | chinese | no |
| [personal_space2](https://github.com/qirien/personal-space2) | 6.99/7 | GPL-3.0 program/text | 31 | 14,395 | - | no |
| [wanted_dragon](https://github.com/qirien/wanted-dragon) | 6 (script_version 6.18.3) | CC BY-NC-SA 4.0 | 36 | 2,765 | spanish | yes |
| [chemistry_hero](https://github.com/qirien/chemistry-hero) | 6 (2015) | GPL-3.0 | 5 | 38 | - | yes (unfinished: 1 image in repo) |
| [enkindle](https://github.com/qirien/enkindle) | 8 | GPL-3.0 | 45 | 45 | 8 | no |
| [unsupervised](https://github.com/qirien/unsupervised) | 8 | GPL-3.0 | 30 | 755 | - | no |
| [wonder](https://github.com/qirien/Wonder) | 8 | GPL-3.0 | 7 | 98 | - | no |
| [rms_dating_sim](https://github.com/DeadSuperHero/rms-dating-simulator) | 6.99 | GPL-3.0 | 7 | 38 | - | no |
| [arcance](https://github.com/nsobczak/Arcance) | 6.99 | Apache-2.0 | 11 | 307 | - | yes |
| [zero_echoes](https://github.com/Trcmoe/Zero-Echoes) | 8 (8.3.3) | CC-BY-4.0 | 4 | 70 (Chinese) | - | yes |
| [marystory](https://github.com/MaxGog/marystory) | 8 | Apache-2.0 | 9 | 489 (Russian) | - | no |
| [cross_channel](https://github.com/MewX/cross-channel_chinese-localization_project) | 6-era port on 8.5 | Apache-2.0 port code; text and art belong to the commercial original (census only) | 302 | 41,672 (Japanese) | - | no |

Jam corpus (census only, mostly unlicensed): Kirei (2014), CatAdventure (2016), Our Home
(2017), Somnova (2018, CC BY-NC-SA 4.0), Sidetracked (2020, MIT), nano2020 (2020),
malViolence (2023).

Era labels come from commit dates plus evidence in the scripts: the `gui.init` template
(Ren'Py 6.99.12 or later), `config.script_version`, and README version notes.

## What blocks the uncovered games

| Blocker family | Gate games blocked | Typical cause |
|---|---|---|
| Python functions and classes (`py:stmt:FunctionDef`, `ClassDef`, user function calls) | 8 | Stat systems, event dispatchers, quizzes, inventories, achievement helpers |
| Custom screens that run Python (`screen:*:custom-dynamic`) | 7 | Maps, phones, bestiaries, planners with `$`, `for`, `Function()` |
| Custom screens that are static (`screen:*:custom-static`) | 7 | Timed-choice countdowns, simple choice panels |
| Effects outside the core (ImageDissolve, CropMove, blinds/iris, matrixcolor, blur, Movie sprites) | 6 | Eye-open/close masks, wipes with masks, video sprites |
| Ren'Py runtime APIs (`renpy.curry`, `renpy.register_statement`, `python early`) | 7 | Screen actions and creator-defined statements |

Per-game blockers (top features, number of occurrences):

- **learntocoderpg**: `QuizQuestion` class x638, user functions x96, methods on objects, `blinds` x58, 38 functions, static and dynamic custom screens.
- **ksre**: `with persistent.charachange` (a transition chosen at runtime) x8,515, `MultipleTransition`, `ImageDissolve`, custom warpers, 72 custom screen calls.
- **personal_space / personal_space2**: a Python event dispatcher and stat system, a day planner, custom screens, custom `{emoji}` tag.
- **enkindle / unsupervised / wonder**: point-and-click rooms and maps built from custom screens and Python helpers.
- **rms_dating_sim**: only a static `countdown` screen (timed menu).
- **marystory**: a static choice screen, `Movie` sprites, `blur`/`matrixcolor`, `blinds`, `irisout`.
- **cross_channel**: `ImageDissolve` x89, a Python lookback (history) system, `im.Crop` frames inside ATL.

## Feature frequency (gate corpus, story files only)

Story files exclude the template UI files (`gui.rpy`, `screens.rpy`, `options.rpy`) and `tl/`.

| Feature | Games | Occurrences | Feature | Games | Occurrences |
|---|---|---|---|---|---|
| say | 15 | 104,001 | `with` statement | 11 | 20,911 |
| label | 15 | 1,850 | show | 13 | 18,632 |
| menu | 15 | 886 | scene | 15 | 3,800 |
| `$` line | 15 | 4,989 | hide | 11 | 1,776 |
| define | 15 | 712 | play / stop | 10 / 7 | 3,196 / 1,764 |
| jump | 14 | 1,015 | voice | 1 | 10,687 |
| if | 14 | 1,826 | pause statement | 4 | 828 |
| call | 11 | 15,556 | `renpy.pause()` | 6 | 135 |
| menu choice `if` | 8 | 280 | nvl dialogue | 5 | 2,900 |
| while | 6 | 38 | window | 6 | 65 |
| default | 10 | 413 | image statement | 12 | 1,086 |
| `init python` | 10 | 85 | transform statement | 9 | 105 |
| label parameters / call arguments | 6 / 5 | 22 / 14,422 | ATL warpers (linear/ease) | 8 | 842 |
| `renpy.input()` | 5 | 18 | ATL `alpha` / `zoom` | 9 / 5 | 599 / 332 |
| text interpolation `[var]` | 10 | 2,076 | ATL `on` events | 7 | 40 |
| text tags (any) | 10 | 1,204 | layeredimage | 2 | 17 |

Text tags by number of games: `b` 7, `i` 5, `w` 4, `font` 4, `a` 3, `size` 3, `color` 2,
`nw` 2, `p`/`fast`/`s`/`cps`/`vspace`/`space`/`image` 1 each, plus custom tags
(`bt`, `icon`, `emoji`, `alt`) in 3 games.

Transitions by number of games: `dissolve` 14, `fade` 10, `move`/`movein*`/`moveout*` 8,
`vpunch` 6, `Dissolve()` 6, `Fade()` 6, `Pause()` 5, `hpunch` 5, `ImageDissolve` 3,
`pixellate` 2, `blinds`/`zoomin`/`irisin`/`irisout`/`CropMove`/`Move` 2 each, `ease*` 1,
`wipe*` 1 (657 uses in cross_channel), `slide*` 1.

Asset formats (files in the game folders, gate corpus): PNG in 14 games (3,596 files),
JPG 11 (468), OGG 8 (185), TTF 8 / OTF 5, MP3 5 (97), WAV 2, Opus 2, WebP 1, WebM 1 (3
files), MKV 1 (176 files, ksre). Several repos omit their commercial or large assets
(cross_channel, chemistry_hero), so counts are lower bounds.

## Supported subset (decision)

The full list of feature keys is [`subset.json`](../../tools/census/subset.json) `supported`
(259 patterns). In summary:

**Phase 02 list, kept as written:** `label`, `jump`, `call`/`return`, say with Character
definitions and name colors, `menu` with conditions, `if`/`elif`/`else`, `while`, `$`
assignments, `define`/`default`, `scene`/`show`/`hide` with tags, attributes, `as`,
`behind`, `zorder`, `with` (dissolve, fade, pixellate, move, slide), `play`/`stop`/`queue`
music and sound, `pause`, `nvl`, `window`, `centered`, `extend`, text tags `b i color w p
nw fast`, ATL positions/`xalign`/`yalign`/`linear`/`ease`/`zoom`/`alpha`/`repeat`.

**Added because the data shows they are common and cheap:**

- Control flow: named menus, `menu` `set`, label parameters and call arguments,
  `call`/`jump expression`, local labels (`.name`), `show expression`, `show text`, say
  with `with`, temporary say attributes (`e @ happy "..."`).
- Transitions: `vpunch`/`hpunch`, `ease*`, `wipe*`, `push*`/`PushMove`, `Pause()`,
  `Move(...)` shakes, names created by `define.move_transitions`.
- Expressions (phase 02 safe subset, extended): subscript assignment, named stores
  (`default ch1.x`), builtins `len str int min max abs round set list`, string and
  container methods `strip lower upper capitalize title add append remove discard pop get`.
- Ren'Py APIs with a direct engine equivalent: `renpy.pause`, `renpy.input` (PSP on-screen
  keyboard), `renpy.random.randint/choice/random`, `renpy.notify`, `renpy.full_restart`,
  `renpy.block_rollback`, `renpy.save_persistent`, `renpy.force_autosave`,
  `renpy.has_label`, `renpy.showing`, `renpy.say`, `renpy.call`, `renpy.jump`,
  `renpy.music.play/stop/set_volume`, `renpy.sound.play/stop`, `renpy.movie_cutscene` (PSMF,
  already in scope per plan). `renpy.variant`, `renpy.android`, and similar fold to PSP
  constants; `config.screen_width/height` fold to the source resolution.
- Characters: `DynamicCharacter`, `dynamic=`, click-to-continue `ctc`/`ctc_position`,
  `voice_tag`. Style arguments (`what_*`, `who_*`, `window_*`, `show_*`) map to the
  native text box where an equivalent exists; otherwise the build prints a warning and drops
  them.
- Images resolved at build time: `Image()`, `im.*` (Scale, FactorScale, Composite, Crop,
  MatrixColor...), `Solid`, `Null`, `Composite`/`LiveComposite`, `Crop`/`LiveCrop`, `Frame`
  with constant arguments, `Position(...)` and static `Transform(...)` placements, and the
  cookbook loop that defines an image per file (`idiom:auto-images`, expanded from the
  file listing).
- ATL: `on` events, parameterised transforms, `parallel`/`block`/`choice`/`time`,
  `crop`, `size`, `rotate`, `additive`, `offset`, `xcenter`/`ycenter`.
- Text tags: `size`, `font` (the asset pipeline builds a bitmap font per face), `s`, `u`,
  `cps`, `vspace`, `space`, `a` (rendered as plain text).
- Audio: `volume`, custom channels from `renpy.music.register_channel`, file names found
  by Ren'Py's automatic `audio/` namespace.

**Ignored (replaced by the native vnpsp UI, informational only):** the template files
`gui.rpy`/`screens.rpy`/`options.rpy`, `config.*`/`gui.*`/`build.*`/`style.*`
settings, `style` statements, the default screens (`say`, `choice`, `nvl`, `save`,
`preferences`, ...), custom screens that are never invoked from the story, and
`Gallery`/`MusicRoom` extras.

**Out of the subset, reported as build errors with file and line:** Python
`def`/`class`/`import`/`for`/`try` outside the idioms above, calls to user functions,
custom screens shown from the story, creator-defined statements, `python early`,
transitions stored in variables, `ImageDissolve`/`AlphaDissolve`/`CropMove`/blinds/iris/zoom,
`layeredimage`, `ConditionSwitch`, `Movie` sprites, `matrixcolor`, `blur`, shaders, custom
text tags, custom ATL warpers and `function` statements, and `onlayer`.

## Target games

| # | Game | License | Ren'Py | Size | Why |
|---|---|---|---|---|---|
| 1 | **the_question** | MIT | 8.5 | 71 lines, 6 labels | Reference game shipped with Ren'Py. Its translations cover CJK (Japanese, Korean, Simplified and Traditional Chinese), which exercises build-time language selection and CJK fonts. |
| 2 | **zero_echoes** | CC-BY-4.0 | 8.3.3 | 70 lines (Chinese) | CJK text, `renpy.input` password entry (PSP on-screen keyboard), automatic audio names, MP3/OGG/WAV mix. |
| 3 | **arcance** | Apache-2.0 | 6.99 | 307 lines, 31 labels | Ren'Py 6-era script with old-style side images built from `im.Composite`/`im.FactorScale` and `config.screen_*` arithmetic, which tests build-time image baking. Long enough for the 50-step rollback check. |
| Alt | wanted_dragon | CC BY-NC-SA 4.0 (non-commercial) | 6.18 | 2,765 lines | Largest covered game: NVL mode, side images, move transitions, Spanish translation, `idiom:auto-images`. Use it as a stretch target if a non-commercial license is acceptable. |

chemistry_hero is covered but unfinished (38 lines, 1 image in the repo), so it is not a target.

## Options for the gate (ranked)

1. **Recommended: accept the core subset and narrow the plan's claim.** State that
   rpy2psp targets dialogue-driven VNs (kinetic and branching novels without minigames or
   custom screens), and change the gate to "three target games fully covered; corpus
   coverage measured and reported" instead of 10 of 15. The data shows that the remaining
   games need Ren'Py's Python runtime, which a 64 MB PSP with an ahead-of-time compiler
   should not try to reproduce. Phase 02 can add static custom screens later (+1 game).
2. Add static custom screens and the extra effects in phases 02 and 04 (7 of 15). This
   costs roughly a week of compiler work and some GE work, mostly mask transitions. It
   still misses the gate.
3. Ship a Python interpreter on the PSP (Stackless Python 2.5 has run there, see
   prior-art) and reimplement Ren'Py's screen language and display runtime. This reaches
   11 to 15 of 15 at multi-month cost, conflicts with the workspace rule against
   interpreter-first ports, and is the same scope as a full Ren'Py port. Not recommended.

## Limitations

- Selection bias: source-available games skew towards developer-authors (five are by
  one studio, qirien). Free itch.io VNs that ship `.rpy` were not sampled because
  downloads need a browser flow and the licenses are unstated. The jam corpus suggests
  the picture is similar (1 of 7 covered).
- Static analysis only. Covered games have not been played through, so runtime-only
  behaviour (for example image names built from variables) can still surface in phase 02.
- The parser is standalone; see the [census tool README](../../tools/census/README.md) for
  its limits.
- `.rpyc`/`.rpa`-only distributions were not read.
