# Supported Ren'Py features

rpy2psp supports the part of Ren'Py that dialogue-driven visual novels use. Anything else stops the build with the file, the line and a feature name in brackets, for example `[screen:show:custom]`. The machine-readable list is [`tools/census/subset.json`](../../tools/census/subset.json); this page describes it.

## Story statements

| Feature | Supported |
|---|---|
| Dialogue | Narrator, characters, speaker strings, `extend`, `centered`, image attributes on a line (`e happy "..."`), temporary attributes, `with` on a line, NVL characters |
| `label`, `jump`, `call`, `return`, `pass` | Yes, including label parameters, call arguments, `jump expression`, `call expression` and local labels (`.name`) |
| `menu` | Choices with `if` conditions, a caption line, `set`, named menus |
| `if` / `elif` / `else`, `while` | Yes |
| `define`, `default`, `persistent` | Yes, including named stores (`define mystore.x = ...`) |
| `image` | Files, colours, aliases, ATL images, `Image(...)`, `im.*` operations, `Solid`, `Null`, `Composite`, `LiveComposite`, `Crop`, `Frame`, and images found automatically in `images/` |
| `scene`, `show`, `hide` | `as`, `behind`, `zorder`, `at`, `with`, ATL blocks, `show expression`, `show text` |
| `with` | `dissolve`, `fade`, `pixellate`, `move`, `movein*`/`moveout*`, `ease*`, `slide*`, `wipe*`, `push*`, `vpunch`, `hpunch`, `None`, and `Dissolve`, `Fade`, `Pixellate`, `MoveTransition`, `Move`, `PushMove`, `Pause` objects |
| `transform` and ATL | Positions, alignment, anchors, offsets, zoom, alpha, rotate, crop, size, additive; `linear`, `ease`, `easein`, `easeout`; `pause`, `repeat`, `parallel`, `block`, `choice`, `on`, `time`, `include`; transform parameters |
| `play`, `queue`, `stop`, `voice` | `music`, `sound`, `voice`, `audio` and custom channels; `fadein`, `fadeout`, `loop`, `noloop`, `if_changed`, playlists, `audio.*` names, and `renpy.music.set_volume` |
| `pause`, `window`, `nvl` | `pause` with or without a time, `window show/hide/auto`, `nvl clear/show/hide` |
| Translations | `translate` blocks and strings; pick one with `--language` |

## Text

Text tags `b`, `i`, `u`, `s`, `color`, `size`, `font`, `a` (shown, not followed), `cps`, `w`, `p`, `nw`, `fast`, `space`, `vspace`, and `[variable]` interpolation (`[name]`, `[name.attr]`, `[name[key]]`) with the `!t !u !l !c !q !r !s !i` flags and format specs. Line breaking follows the CJK rules (no closing punctuation at a line start), so Chinese, Japanese and Korean games work when their font is available.

Character options: `color`, `who_*`, `what_*`, `window_*`, `image`, `kind`, `dynamic`, `name`, `ctc`, `ctc_position`, `voice_tag`, `show_side_image`, `show_two_window`. Side images come from `side <tag> <attributes>` images.

## Python

Only expressions and simple statements, compiled ahead of time; no Python runs on the PSP.

- Values: numbers, strings, booleans, `None`, lists, tuples, dicts and sets, with Python's own arithmetic, comparisons, `and`/`or`/`not`, indexing and slicing, `%` formatting and `format()` specs.
- Statements in `$` lines and `python:` blocks: assignment (to names, store attributes, persistent fields and subscripts) and augmented assignment.
- Functions: `len`, `str`, `int`, `min`, `max`, `abs`, `round`, `set`, `list`, `_()`, and the methods `strip`, `lower`, `upper`, `capitalize`, `title`, `add`, `append`, `remove`, `discard`, `pop`, `get`. `lower`, `upper`, `title` and `capitalize` change ASCII letters only.
- Ren'Py functions: `renpy.pause`, `renpy.input` (on-screen keyboard, printable ASCII), `renpy.random.randint/choice/random`, `renpy.notify`, `renpy.say`, `renpy.call`, `renpy.jump`, `renpy.has_label`, `renpy.showing`, `renpy.music.play/stop`, `renpy.sound.play/stop`, `renpy.full_restart`, `renpy.block_rollback`, `renpy.save_persistent`, `renpy.force_autosave`, `renpy.variant`, `define.move_transitions`.

## Replaced by the engine

The game's own screens (`screens.rpy`), GUI settings and `options.rpy` are not converted. vnpsp draws its own text box, choice menu, main and game menus, save and load screens with 30 slots and thumbnails, history, settings, and a quick menu on the buttons. Showing or calling one of Ren'Py's built-in screens by name (`save`, `load`, `preferences`, ...) is skipped with a warning; players reach those through START. `Gallery` and `MusicRoom` setups are ignored.

## Not supported

- Python beyond the list above: `def`, `class`, `for`, `import`, user functions, `python early`, creator-defined statements.
- Custom screens (screen language), static or dynamic, and `renpy.call_screen` / `show screen` of them.
- `ImageDissolve`, `CropMove`, `blinds`, `squares`, `irisin/out`, `zoomin/out`, `MultipleTransition`, transitions chosen at runtime, `layeredimage`, `ConditionSwitch`, `Movie` sprites, `matrixcolor`, `blur`, `onlayer`.
- Movie cutscenes (`renpy.movie_cutscene`) are skipped with a log line.
- `window show dissolve` and `nvl show dissolve` switch at once, without the transition.
- Live2D and video playback.

Coverage of a set of games: `rpy2psp coverage --manifest tools/census/games.json --games-dir <dir>`. In the 15-game census corpus, 5 games compile with no errors ([docs/dev/census.md](../dev/census.md)). Games that were played on the engine are listed in [`compat/`](../../compat/).
