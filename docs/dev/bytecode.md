# VN bytecode (`script.vnb`) and asset manifest

`rpy2psp compile` writes two files:

- `script.vnb` is the compiled story that the PSP engine (vnpsp) runs.
- `manifest.json` lists every asset the story references, as build recipes. The asset pipeline turns it into the asset pack.

The source of truth is [`rpy2psp/compiler/bytecode.py`](../../rpy2psp/compiler/bytecode.py), which holds the opcode table, the writer, and the reader. A reference interpreter lives in [`tests/compiler/refvm.py`](../../tests/compiler/refvm.py). This page explains the format and its semantics.

All integers are little-endian.

- `str` is a u32 index into the string table. `0xFFFFFFFF` means none.
- Table references (`disp`, `transform`, `transition`, `show`, `audio`, `asset`) are u16 indexes. `0xFFFF` means none.

## Container

| Offset | Field |
|---|---|
| 0 | `char[4]` magic `VNB1` |
| 4 | u16 version (2; version 1 stored PUSH_FLOAT as f32) |
| 6 | u16 reserved (0) |
| 8 | u32 section count |
| 12 | section table: `char[4] id`, u32 offset (from file start), u32 size |

Sections start on 4-byte boundaries. A reader must look sections up by id, not by position.

| Id | Content |
|---|---|
| `HEAD` | u32 init address, u32 defaults address, u32 start address, u32 flags (0), f32 source width, f32 source height |
| `CODE` | instruction stream |
| `SYMS` | u32 count; per variable: `str` name, u8 kind (0 store, 1 persistent), u8 + u16 padding |
| `LABL` | u32 count; per label, sorted by name: `str` name, u32 code address |
| `CHRS` | u32 count; 38-byte characters (below) |
| `IMGS` | u32 count; per image: `str` full name (`"sylvie green smile"`), `disp` |
| `DISP` | u32 count; displayables (below) |
| `TRFM` | u32 count; per transform: u32 byte length, ATL program (below) |
| `TRAN` | u32 count; transitions (below) |
| `SHOW` | u32 count; show records (below) |
| `AUDI` | u32 count; play records (below) |
| `CHAN` | u32 count; per channel: `str` name, u8 mixer (0 music, 1 sfx, 2 voice), u8 loops by default |
| `ASET` | u32 count; per asset: `str` key (the asset pack key), u8 kind (0 image, 1 music, 2 sfx, 3 voice, 4 font, 5 movie) |
| `META` | `str` game title (`config.name`), `str` game version (`config.version`); either may be none |
| `STRS` | u32 count, u32 offsets[count] relative to the end of the offset array, then UTF-8 strings each ending in NUL |

Positions are kept in the game's own resolution (`HEAD` source size). The engine scales uniformly by `min(480 / w, 272 / h)` and letterboxes the rest. In transforms, integer pixel values and pixel offsets are in source pixels.

## Program model

- **Values.** None, bool, int (i32), float (f32), string, list, tuple (an immutable list), dict, and set. Truthiness, equality, `+ - * / // %`, `in`, and slicing follow Python. `str % value` supports `%s %d %i %f %%`.
- **Variables.** Variables are symbols. A store variable read before it is ever assigned is a runtime error. A persistent variable (`persistent.x`) reads as None until it is set. Named stores appear as dotted names (`ch1.score`).
- **Boot.** On boot, run the `init` routine (`HEAD` init address) to `RETURN`. For a new game, run the `defaults` routine and then jump to `start`. When loading a save, run `init`, restore the saved variables, then run `defaults`. `STORE_DEFAULT` writes only when the variable is unset, which fills in variables added after the save was made.
- **Call stack.** `CALL` pushes a frame holding the return address. `BIND` gives a label parameter its value: it remembers the variable's old value in the current frame, and `RETURN` restores every value its frame remembered (Ren'Py's dynamic scoping). `RETURN` with an empty stack ends the game and returns to the main menu. `return expr` stores the value in `_return` before `RETURN`.
- **Labels with parameters.** These have two entry points. The label table points at the default entry, which pushes the default values. A call with arguments pushes the values itself and calls the bound entry directly.

## Instructions

Each instruction is a u8 opcode followed by its operands. Operand codes:

| Code | Meaning |
|---|---|
| `b` | u8 |
| `h` | u16 table index |
| `v` | u16 symbol |
| `i` | i32 |
| `f` | f32 |
| `d` | f64 |
| `s` | u32 string |
| `a` | u32 code address |
| `S` | u8 count, then that many u32 strings |
| `M` | u8 count, then that many (u32 string, u32 address) pairs |

| Op | Name | Operands | Effect |
|---|---|---|---|
| 0x00 | NOP | - | |
| 0x01-0x03 | PUSH_NONE / PUSH_TRUE / PUSH_FALSE | - | push a constant |
| 0x04 | PUSH_INT | i | |
| 0x05 | PUSH_FLOAT | d | f64, so constants keep Python's double precision |
| 0x06 | PUSH_STR | s | |
| 0x07 | LOAD | v | push variable |
| 0x08 | STORE | v | pop into variable |
| 0x09 | STORE_DEFAULT | v | pop; store only if unset (persistent: unset or None) |
| 0x0A | POP | - | |
| 0x0B | DUP | - | |
| 0x0C-0x0F | BUILD_LIST / BUILD_TUPLE / BUILD_DICT / BUILD_SET | h n | pop n items (n key/value pairs for a dict) and push a container |
| 0x10 | SUBSCR | - | `c k -> c[k]` |
| 0x11 | SLICE | - | `c lo hi -> c[lo:hi]` (None means open) |
| 0x12 | STORE_SUBSCR | - | `c k v ->` and `c[k] = v` |
| 0x13 | BINARY | b op | ADD SUB MUL DIV FLOORDIV MOD (0-5) |
| 0x14 | UNARY | b op | NEG POS NOT (0-2) |
| 0x15 | COMPARE | b op | EQ NE LT LE GT GE IN NOTIN IS ISNOT (0-9) |
| 0x16 | JUMP | a | |
| 0x17 / 0x18 | JUMP_IF_FALSE / JUMP_IF_TRUE | a | pop the condition, jump if it matches |
| 0x19 / 0x1A | JUMP_IF_FALSE_OR_POP / JUMP_IF_TRUE_OR_POP | a | `and` / `or`: if it matches, keep it and jump; otherwise pop |
| 0x1B | CALL_FN | b fn, b argc | builtin function (table below); pops args, pushes result |
| 0x1C | CALL_METHOD | b method, b argc | `obj args... -> result` |
| 0x1D | CALL | a | push frame, jump |
| 0x1E / 0x1F | CALL_DYNAMIC / JUMP_DYNAMIC | - | pop a label name and look it up in `LABL` |
| 0x20 | RETURN | - | see the program model |
| 0x21 | BIND | v | pop a value into a label parameter (dynamic scope) |
| 0x22 | SAY | h char, s text, h transition, b flags | show dialogue; flags bit 0 = wait for the player (otherwise continue at once, as for a menu caption) |
| 0x23 | SAY_DYNAMIC | h char, h transition, b flags | as SAY, with the text popped from the stack |
| 0x24 | SAY_ATTRS | h char, b temporary, S attributes | change the attributes of the character's image tag (`e happy "..."`); `temporary` restores them after the next line; for `extend`, applies to the last speaker |
| 0x25 | MENU | M choices | pop one condition per choice (first choice deepest); show the true ones; jump to the chosen address. If no choice is visible, continue |
| 0x26 | PAUSE | - | pop a delay (None means wait for a click) |
| 0x27 | WITH | h transition | run the transition from the last committed screen to the current one; none means commit without animation (`with None`) |
| 0x28 / 0x2A / 0x2B | SHOW / SCENE / HIDE | h show | SCENE clears the layer first; a none show record is a bare `scene` |
| 0x29 | SHOW_DYNAMIC | h show | pop an image name string (`show expression`); the record supplies the clauses |
| 0x2C | PLAY | h audio | play or queue (record flags) |
| 0x2D | STOP | b channel, f fadeout | fadeout -1 means the channel default |
| 0x2E | VOICE | h asset | play on the voice channel; it stops at the next interaction unless sustained |
| 0x2F | VOICE_SUSTAIN | - | |
| 0x30 | WINDOW | b mode, h transition | modes: show, hide, auto, auto-show, auto-hide |
| 0x31 | NVL | b mode, h transition | modes: clear, show, hide |
| 0x32 | MOVIE | h asset | full-screen cutscene (`renpy.movie_cutscene`) |

Opcodes are numbered in table order with no gaps; `OPCODES` in `bytecode.py` is authoritative.

**Builtin functions (`CALL_FN`), by index:**

| Index | Function |
|---|---|
| 0-10 | `len str int float bool min max abs round set list` |
| 11 | `renpy.random.randint(a, b)` |
| 12 | `renpy.random.choice(seq)` |
| 13 | `renpy.random.random()` |
| 14 | `renpy.input(prompt, default, length)`: on-screen keyboard; returns a string |
| 15 | `renpy.notify(msg)` |
| 16 | `renpy.full_restart()` |
| 17 | `renpy.block_rollback()` |
| 18 | `renpy.save_persistent()` |
| 19 | `renpy.force_autosave()` |
| 20 | `renpy.has_label(name)` |
| 21 | `renpy.showing(name)` |
| 22 | `renpy.music.set_volume(volume, delay, channel index)` |
| 23 | `menu_set_add(container, text)`: add for a set, append for a list (menu `set`) |
| 24 | `tuple` |

**Methods (`CALL_METHOD`), by index:** `strip lower upper capitalize title add append remove discard pop get keys values items count index startswith endswith replace split join`.

## Text

Strings are stored after build-time translation and Unicode NFC normalisation. The engine still handles the following at display time:

- **Text tags:** `{b} {i} {u} {s} {color=} {size=} {font=} {alpha=} {a=}` (shown as plain text), `{w} {w=} {p} {p=} {nw} {fast} {cps=} {vspace=} {space=}`. Any other tag is a build error.
- **Escapes:** `{{` is a literal `{`, `[[` is a literal `[`, and `%%` is `%`.
- **Interpolation:** `[name]`, `[name.attr]`, `[name[key]]`, with `!t !u !l !c !q !r !s !i` flags and a `:format` part. The compiler checks that every interpolated name is a symbol. The engine looks the name up in `SYMS`.

## Characters (`CHRS`, 38 bytes each)

| Field | Type |
|---|---|
| name | `str` (none for the narrator; may hold `[var]` for dynamic names) |
| who colour, what colour | u8[4] RGBA each; all zero means the default |
| kind | u8: 0 ADV, 1 NVL |
| flags | u8: 1 centered, 2 extend, 4 dynamic name |
| image tag | `str`, for say attributes and side images |
| side image | `disp` |
| what prefix, what suffix, who prefix, who suffix | `str` each |
| click-to-continue | `disp` |

Entries 0-4 are built in: narrator, centered, the NVL narrator, extend, and vcentered.

## Images and displayables

An image name is resolved when it is shown. The engine applies Ren'Py's attribute rule:

1. Take the current attributes of the tag, apply the requested attributes (an `-attr` removes one), and pick the defined image that has every requested attribute and the most current ones.
2. A name with no match shows nothing, as Ren'Py does outside developer mode. The compiler warns.

Each displayable (`DISP`) is:

| Field | Type |
|---|---|
| kind | u8 |
| asset | u16 |
| colour | RGBA |
| width, height | f32 each (source pixels) |
| transform | u16 |
| child | u16 |
| text | `str` |
| borders | f32[4] |

| Kind | Meaning |
|---|---|
| 0 TEXTURE | an image asset |
| 1 SOLID | a colour that fills its area (the screen) |
| 2 NULL | an empty displayable of a fixed size |
| 3 FRAME | a nine-slice of an asset with borders left, top, right, bottom |
| 4 ATL | an animated image: its transform holds `child` instructions |
| 5 TEXT | `show text`; the text is drawn with the dialogue font |
| 6 TRANSFORMED | `child` placed by `transform` (for example `Image("x.png", xalign=0.5)`) |

Image recipes such as `im.Composite`, `im.FactorScale`, `im.Crop`, `im.MatrixColor`, `Composite`, and `Crop` are baked into a single texture by the asset pipeline.

## Transforms (`TRFM`)

An ATL program is a sequence of instructions. Property values are lists of components, and each component is u8 kind plus f32 value:

- kind 0: a float or fraction;
- kind 1: an integer number of source pixels;
- kind 2: None.

| Code | Instruction | Payload |
|---|---|---|
| 0x01 | SET | u8 count, then `(u8 property, u8 n, n components)` |
| 0x02 | INTERP | u8 warper, f32 duration, then the same property list as SET |
| 0x03 | PAUSE | f32 |
| 0x04 | REPEAT | i32 count (-1 forever); restarts the enclosing block |
| 0x05 | BLOCK | u32 length, then nested instructions |
| 0x06 | PARALLEL | u8 count, then `(u32 length, instructions)` each |
| 0x07 | CHOICE | u8 count, then `(f32 weight, u32 length, instructions)` each |
| 0x08 | ON | u8 count, u32 event names, u32 length, instructions |
| 0x09 | TIME | f32 |
| 0x0A | CHILD | u16 displayable, u16 transition |
| 0x0B | EVENT | u32 name |

- **Warpers:** 0 linear, 1 ease, 2 easein, 3 easeout.
- **Properties:** 0 xpos, 1 ypos, 2 xanchor, 3 yanchor, 4 xalign, 5 yalign, 6 xoffset, 7 yoffset, 8 zoom, 9 xzoom, 10 yzoom, 11 alpha, 12 rotate, 13 additive, 14 xsize, 15 ysize, 16 crop (4 components), 17 xcenter, 18 ycenter. `pos`, `anchor`, `align`, `offset`, `size`, and `xysize` are expanded into their x and y parts at build time.
- **Events:** `on` may name start, show, hide, replace, replaced, update, and the button events. Button events never fire for story images.
- **Builtins:** `left`, `right`, `center`, `truecenter`, `top`, `topleft`, `topright`, `offscreenleft`, `offscreenright`, `default`, and `reset` are compiled from the prelude in `compile.py`, so the table always contains them.

## Transitions (`TRAN`)

Each record is:

| Field | Type |
|---|---|
| kind | u8 |
| warper or flags | u8 |
| params | f32[6] |
| colour | RGBA |
| enter, leave | u16 transform each |
| enter warper, leave warper | u8 each |

| Kind | Params |
|---|---|
| 1 DISSOLVE | time |
| 2 FADE | out, hold, in, plus colour |
| 3 PIXELLATE | time, steps |
| 4 MOVE | delay, plus warpers and enter/leave transforms (`move`, `ease`, `moveinleft`, ...) |
| 5 CROPMOVE | time, mode: wiperight, wipeleft, wipeup, wipedown, slideright, slideleft, slideup, slidedown, slideawayright, slideawayleft, slideawayup, slideawaydown |
| 6 PUSHMOVE | time, direction: right, left, up, down |
| 7 SHAKE | x0, y0, x1, y1, period, delay; flags 1 bounce, 2 repeat (`vpunch`, `hpunch`, `Move`) |
| 8 PAUSE | time |

`show x with t` compiles to `WITH none`, `SHOW`, `WITH t`, as in Ren'Py.

## Show records (`SHOW`)

Each record is, in order:

| Field | Type |
|---|---|
| tag | `str` |
| attribute count, attributes | u8, then `str` each |
| at-list count, transforms | u8, then u16 each |
| as | `str` |
| behind count, tags | u8, then `str` each |
| zorder | i32 |
| has zorder | u8 |
| layer | `str` (always `master`) |
| ATL block | u16 transform |
| displayable | u16; set for `show text` and constant `show expression`, otherwise none |

## Play records (`AUDI`)

Each record is:

| Field | Type |
|---|---|
| channel | u8 |
| flags | u8: 1 loop, 4 if_changed, 8 queue |
| fadein | f32 |
| fadeout | f32 (-1 means the default) |
| volume | f32 |
| asset count, assets | u8, then u16 each (a playlist) |

Channels 0-3 are music, sound, voice, and audio. Channels registered with `renpy.music.register_channel` follow.

## Manifest (`manifest.json`)

```json
{
  "format": "rpy2psp-manifest/1",
  "language": null,
  "source_size": [1280, 720],
  "config": {"screen_width": 1280, "screen_height": 720, "outlines": false, "name": "..."},
  "fonts": {"gui.text_font": "DejaVuSans.ttf"},
  "text_fonts": [],
  "glyphs": "every character the story can display, plus printable ASCII",
  "assets": {"img/<sha1-16>": {"kind": "image", "recipe": {"op": "file", "path": "images/bg club.jpg"}},
             "snd/music/illurock.opus": {"kind": "music", "path": "illurock.opus"}},
  "scenes": {"start": ["img/...", "snd/music/illurock.opus"]}
}
```

- **Image recipes:** `file`, `solid`, `null`, `composite` (size, parts `[x, y, recipe]`), `scale`, `factor`, `crop` (rect), `flip`, `grayscale`, `sepia`, `alpha`, `matrix` (4x5 colour matrix), and `frame`.
- **`scenes`:** maps each top-level label to the assets its statements use. The asset pack stores them together so the engine can prefetch them per scene.

## Compatibility notes

- **Translation IDs.** These are computed the way Ren'Py's restructurer computes them. They are checked against all 70 dialogue translations of `the_question` in Japanese, Korean, Simplified Chinese, and Traditional Chinese. The build selects one language with `--language`.
- **`.rpyc` files.** These are decoded by a restricted unpickler and turned back into source lines. Arcance's Ren'Py 6.99 `.rpyc` files compile to the same instructions as its `.rpy` files. The only difference is the trailing `return` that Ren'Py adds to each compiled file.
