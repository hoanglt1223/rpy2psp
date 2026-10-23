"""Builds the engine test game: a self-written Ren'Py script that touches every
engine feature, with generated images and sounds and a system font, compiled
and packed into an output folder (script.vnb + data.pak).

    python -m tests.engine.make_test_game <out dir>
"""

import os
import shutil
import sys
import tempfile

import numpy as np
import soundfile
from PIL import Image

from rpy2psp.assets.pack import PackOptions, build_pack
from rpy2psp.compiler.compile import compile_game
from rpy2psp.compiler.reader import load_game

SYSTEM_FONTS = [r"C:\Windows\Fonts\arial.ttf", r"C:\Windows\Fonts\segoeui.ttf",
                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf"]

# 441 Hz at 44.1 kHz is exactly 100 samples per period, so a 2 s track loops without a seam.
MUSIC_HZ = 441
MUSIC_SECONDS = 2

SCRIPT = r'''
define config.screen_width = 1280
define config.screen_height = 720
define gui.text_font = "body.ttf"
define e = Character("Eve", color="#ffcc88", image="eve", what_prefix="«", what_suffix="»")
define b = Character("Bob", who_color="#88ccff", what_color="#ddffdd")
define n = Character("Nora", kind=nvl)
define p = Character("player_name", dynamic=True)
define slow_fade = Fade(0.3, 0.2, 0.3, color="#204060")
define quick = Dissolve(0.25)
default player_name = "Pat"
default visits = 0
default bag = ["key"]
default stats = {"str": 3, "luck": 1}
default seen_set = set()
default persistent.runs = 0

image bg hall = "images/bg hall.png"
image bg yard = "images/bg yard.png"
image eve happy = "images/eve happy.png"
image eve sad = "images/eve sad.png"
image side eve happy = "images/side eve.png"
image side eve sad = "images/side eve.png"
image flash = Solid("#ffffff")

transform bob_up:
    xalign 0.5 yalign 1.0
    linear 0.5 yoffset -20
    linear 0.5 yoffset 0
    repeat

transform pulse:
    parallel:
        alpha 0.5
        linear 0.4 alpha 1.0
    parallel:
        zoom 0.9
        easein 0.4 zoom 1.0

transform tilt:
    xalign 0.3 yalign 0.9 rotate 8

label start:
    $ persistent.runs += 1
    play music "audio/theme.ogg"
    scene bg hall with fade
    "Welcome. This is the vnpsp engine test."
    show eve happy at left with dissolve
    e "Hello, [player_name]! {b}Bold{/b}, {i}italic{/i}, {u}under{/u}, {s}struck{/s}."
    e sad "A {color=#ff6060}red{/color} word, a {size=+8}big{/size} one, and {alpha=0.5}faint{/alpha} text."
    e @ happy "Temporary smile.{w=0.3} Then a pause.{w} Click to go on."
    extend " And this part extends the line."
    b "Escapes: {{braces}} and [[brackets]], 100%% sure."
    play sound "audio/click.wav"
    show eve happy at bob_up
    b "Text speed changes: {cps=10}slow{/cps} then {fast}instant from here."
    show flash at pulse
    with quick
    hide flash
    show eve sad at tilt with move
    centered "{size=+10}Centered text{/size}"
    $ name = renpy.input("Your name?", default="Pat", length=12)
    $ player_name = name.strip() or "Pat"
    p "My name is [player_name!u] and visits are [visits:03d]."
    call counting(3)
    "Back from the call, _return is [_return]."
    $ roll = renpy.random.randint(1, 6)
    $ pick = renpy.random.choice(["a", "b", "c"])
    "Random roll [roll], pick [pick]."
    jump choose

label counting(times=1):
    $ visits += times
    "Counting [times] times; visits now [visits]."
    return visits * 2

label choose:
    menu:
        set seen_set
        "Which way?"
        "To the yard":
            jump yard
        "Stay in the hall" if visits >= 3:
            $ stats["luck"] += 1
            jump hall
        "Hidden option" if visits > 100:
            jump yard

label yard:
    scene bg yard with slow_fade
    show eve happy at right with easeinright
    e "The yard. Luck is [stats[luck]], strength [stats[str]]."
    with vpunch
    with wiperight
    scene bg hall with slideleft
    scene bg yard with pushright
    scene bg hall with pixellate
    jump nvl_part

label hall:
    show eve sad at center
    e "Still in the hall. Bag holds [bag[0]] and luck is [stats[luck]]."
    $ bag.append("map")
    show text "{size=+6}Chapter Two{/size}" at truecenter with dissolve
    pause 0.5
    hide text with dissolve
    jump nvl_part

label nvl_part:
    window hide
    n "NVL mode: lines pile up on one page."
    n "A second NVL line."
    nvl clear
    n "A fresh page after nvl clear."
    window auto
    stop music fadeout 0.5
    $ renpy.notify("Saved runs: " + str(persistent.runs))
    pause
    menu:
        "Again?"
        "Go back to the choice" if len(seen_set) < 2:
            jump choose
        "Finish":
            pass
    $ nseen = len(seen_set)
    "The end. Bag: [bag], set size [nseen]."
    return
'''


def system_font():
    for path in SYSTEM_FONTS:
        if os.path.isfile(path):
            return path
    raise FileNotFoundError("no system TrueType font found (tried %s)" % ", ".join(SYSTEM_FONTS))


def gradient(w, h, c0, c1, alpha=False):
    y = np.linspace(0, 1, h)[:, None, None]
    x = np.linspace(0, 1, w)[None, :, None]
    rgb = (np.array(c0) * (1 - y) + np.array(c1) * y) * (0.75 + 0.25 * x)
    img = np.concatenate([rgb, np.full((h, w, 1), 255.0)], axis=2)
    if alpha:
        yy, xx = np.mgrid[0:h, 0:w]
        inside = ((xx - w / 2) / (w / 2)) ** 2 + ((yy - h / 2) / (h / 2)) ** 2 <= 1.0
        img[..., 3] = np.where(inside, 255, 0)
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGBA")


def write_game(root):
    game = os.path.join(root, "game")
    os.makedirs(os.path.join(game, "images"))
    os.makedirs(os.path.join(game, "audio"))
    gradient(1280, 720, (40, 60, 120), (200, 180, 140)).convert("RGB").save(os.path.join(game, "images", "bg hall.png"))
    gradient(1280, 720, (30, 120, 40), (180, 220, 250)).convert("RGB").save(os.path.join(game, "images", "bg yard.png"))
    gradient(300, 600, (250, 200, 160), (120, 40, 40), alpha=True).save(os.path.join(game, "images", "eve happy.png"))
    gradient(300, 600, (160, 180, 250), (40, 40, 120), alpha=True).save(os.path.join(game, "images", "eve sad.png"))
    gradient(160, 160, (250, 220, 120), (200, 100, 40), alpha=True).save(os.path.join(game, "images", "side eve.png"))
    t = np.arange(44100 * MUSIC_SECONDS) / 44100.0
    tone = 0.4 * np.sin(2 * np.pi * MUSIC_HZ * t)
    soundfile.write(os.path.join(game, "audio", "theme.ogg"), np.stack([tone, tone], 1), 44100, format="OGG")
    click = 0.5 * np.sin(2 * np.pi * 900 * t[:6000]) * np.linspace(1, 0, 6000)
    soundfile.write(os.path.join(game, "audio", "click.wav"), click, 22050)
    shutil.copy(system_font(), os.path.join(game, "body.ttf"))
    with open(os.path.join(game, "script.rpy"), "w", encoding="utf-8") as f:
        f.write(SCRIPT)
    return game


def build(out_dir, script=None):
    """Writes the test game sources to a temporary folder and its converted form to out_dir."""
    src = tempfile.mkdtemp(prefix="vnpsp-testgame-")
    try:
        write_game(src)
        if script is not None:
            with open(os.path.join(src, "game", "script.rpy"), "w", encoding="utf-8") as f:
                f.write(script)
        comp = compile_game(src)
        if comp.errors:
            raise RuntimeError("test game does not compile:\n" + "\n".join(str(x) for x in comp.errors))
        os.makedirs(out_dir, exist_ok=True)
        with open(os.path.join(out_dir, "script.vnb"), "wb") as f:
            f.write(comp.bytecode())
        manifest = comp.manifest()
        g = load_game(src)
        try:
            data, _ = build_pack(g, manifest, PackOptions())
        finally:
            g.close()
        with open(os.path.join(out_dir, "data.pak"), "wb") as f:
            f.write(data)
        return comp
    finally:
        shutil.rmtree(src, True)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: python -m tests.engine.make_test_game <out dir>")
    build(sys.argv[1])
    print("wrote", sys.argv[1])
