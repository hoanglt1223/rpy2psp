# Prior art: Ren'Py on PSP and console visual-novel engines

Searched on 2026-10-23 with web search and `gh search repos` (queries: `renpy psp`,
`renpy vita`, `renpy 3ds`, `renpy nds`, `renpy gba`, `renpy switch homebrew`,
`renpy converter`, `rpy parser`, `visual novel psp engine`, `onscripter psp`, `vnds`,
`unrpyc`, plus the `Ren'Py` language and `renpy` topic listings).

## Verdict

No Ren'Py-to-PSP converter exists, and nothing on the PSP side can be reused as an
engine. The two PSP projects that read Ren'Py-like scripts are unlicensed
interpreters. The plan stands: build `rpy2psp` (PC-side compiler) and `vnpsp` (native
engine) ourselves. Reuse is limited to MIT/WTFPL reference code for reading `.rpyc` and
`.rpa` files.

## PSP and handheld projects

| Project | What it is | License | Status | Fit for rpy2psp |
|---|---|---|---|---|
| [Leave-Us-Alone/RenPy-PSP](https://github.com/Leave-Us-Alone/RenPy-PSP) | Lua clone of Ren'Py on the OneLua v4r1 engine. A line-based reader for "simple rpy scripts" with fixed sprite slots and an `ambience` channel, apparently built for one game (Everlasting Summer). | None (all rights reserved) | One upload on 2022-12-30, 12 stars | Not reusable: no license, interpreter-first, no ATL or real expression handling. Confirms demand. |
| [Semiror78/AG_VN_ENGINE_Portable](https://github.com/Semiror78/AG_VN_ENGINE_Portable) ([GameBrew](https://www.gamebrew.org/wiki/AG_VN_ENGINE_Portable_PSP)) | VN engine for PSP written in PSP Stackless Python, with its own script format | None in repo; GameBrew says "Mixed" | 2023-09, tested on PSP-3000 and PPSSPP | Not Ren'Py-compatible and interpreter-first. Shows that a Python 2.5 runtime fits on a PSP-3000, which is the fallback if we ever need one. |
| [liclac/VNPSP](https://github.com/liclac/VNPSP) | C++ VNDS interpreter for PSP | None | Last push 2014-02-07; the [VNDS wiki](https://github-wiki-see.page/m/BASLQC/vnds/wiki/VNDS-Interpreters) lists it as inactive and not recommended | Different script format, unlicensed. |
| [BadChromium/onscripter-psp](https://github.com/BadChromium/onscripter-psp) | ONScripter (NScripter) for PSP: 480x272 layout, PMF playback, PSP savedata | GPL-2.0 | Active (2026-09), "early development snapshot" | Rejected as a target format: NScripter has no ATL or layered sprites, the engine is GPL-2 and CP936-centric. Worth reading for PMF and savedata integration; do not copy code into MIT files. |
| [BASLQC/vnds](https://github.com/BASLQC/vnds), [ajusa/VNDS-LOVE](https://github.com/ajusa/VNDS-LOVE) | VNDS script standard (DS) and a LÖVE player that also runs on Switch | GPL-2.0, MIT | VNDS last push 2020; VNDS-LOVE 2025 | VNDS lacks animation and effects by design, so it is too small a target for Ren'Py games. |
| [Adastra GBA port](https://liquorice10113.itch.io/adastra-gba-port) | Hand-made GBA port of one Ren'Py VN: Python scripts turn images into C arrays and the script into opcodes, played by a Butano/tonc engine | No source or license published | Prototype | Same architecture as rpy2psp on weaker hardware. It validates the ahead-of-time approach, but there is nothing to reuse. |

## Full Ren'Py ports (not applicable to PSP)

| Project | License | Status | Why it does not transfer |
|---|---|---|---|
| [SonicMastr/renpy-vita](https://github.com/SonicMastr/renpy-vita) | MIT | Last push 2023-05, 115 stars, actively used for Vita ports | Runs real Ren'Py on CPython with the Vita's GPU and 512 MB of RAM. The PSP-3000 has 64 MB and a fixed-function GE, so the same approach does not fit. |
| [uyjulian/renpy-switch](https://github.com/uyjulian/renpy-switch) | None | Archived 2022-01 | Full port, archived in favour of Switchroot Android. |
| [TrueCat17/Ren-Engine](https://github.com/TrueCat17/Ren-Engine) | GPL-3.0 | Active | C++ reimplementation that reads `.rpy` and embeds Python. Desktop only. A useful datapoint: even a reimplementation keeps a Python runtime. |
| Mighty Rabbit / Sekai Project Ren'Py-on-Unity ([report](https://nintendoeverything.com/tag/mighty-rabbit/)) | Commercial | Announced 2016, never public | Console releases of Ren'Py games are rebuilt per title. |

## Readers and converters (reference for phase 02)

| Project | License | Use |
|---|---|---|
| [CensoredUsername/unrpyc](https://github.com/CensoredUsername/unrpyc) | MIT | Reference for decoding `.rpyc` (pickled AST) with a restricted unpickler that never runs game code. MIT, so adapting it is allowed with attribution. |
| [shizmob/rpatool](https://github.com/shizmob/rpatool) (moved to Codeberg) | WTFPL | Reference for the `.rpa` archive format. |
| [Lattyware/unrpa](https://github.com/Lattyware/unrpa) | GPL-3.0 | `.rpa` extractor; read only, do not copy. |
| [cnfatal/rpycdec](https://github.com/cnfatal/rpycdec), RenpyEx | MIT | Alternative `.rpyc`/`.rpa` decoders, useful for cross-checking. |
| [renpy/renpy](https://github.com/renpy/renpy) `renpy/lexer.py` | MIT ([license](https://www.renpy.org/doc/html/license.html)) | The census lexer follows its logical-line rules. The real parser needs Ren'Py's Cython modules (`astsupport.pyx`, `lexersupport.pyx`), so it cannot be imported standalone. |
| [Ren'Py 2 ebook converter](https://renpy2ebook.sourceforge.net) | n/a | Pre-alpha converter to PDF/EPUB. The only other "Ren'Py to a different medium" tool found. |

## Recommendation (ranked)

1. **Build rpy2psp and vnpsp as planned.** Nothing existing can be reused as an engine or compiler.
2. **Base the phase 02 readers on unrpyc (MIT) and rpatool (WTFPL).** Both are license-compatible with this MIT repo; credit them in the file headers.
3. **Read onscripter-psp for PMF cutscenes and PSP savedata.** Its GPL-2 license means it is a design reference only.

## Limitations

- Web results for "Ren'Py PSP" were thin; forums (GBAtemp, Lemma Soft, /r/PSP) were not searched page by page.
- Chinese and Japanese homebrew scenes (for example ONScripter-based PSP releases) were only covered through GitHub search.
