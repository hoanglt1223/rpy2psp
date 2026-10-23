"""Opt-in engine tests on the three target games (game data stays outside the repo).

Point RPY2PSP_TARGETS at the folder holding the_question/, zero_echoes/ and
arcance/ (default %TEMP%/rpy2psp-census/full, as in tests/compiler). The fonts
Ren'Py ships with its SDK are taken from the_question/renpy/common and
the_question/sdk-fonts. Converted data is cached in VNPSP_TARGET_CACHE
(default %TEMP%/vnpsp-targets) and rebuilt when the converter's sources change.
Skipped when the games or the vnpsp executable are missing.
"""

import glob
import hashlib
import os
import tempfile
import unittest

from rpy2psp.assets.pack import PackOptions, build_pack
from rpy2psp.compiler.compile import compile_game
from rpy2psp.compiler.reader import load_game
from tests.compiler.refvm import Program, explore
from tests.engine.harness import ROOT as REPO, reference_trace, run_driver, story_lines, vnpsp_exe

TARGETS = os.environ.get("RPY2PSP_TARGETS", os.path.join(tempfile.gettempdir(), "rpy2psp-census", "full"))
CACHE = os.environ.get("VNPSP_TARGET_CACHE", os.path.join(tempfile.gettempdir(), "vnpsp-targets"))
GAMES = {"the_question": ("the_question/the_question", None, ()),
         "the_question_ja": ("the_question/the_question", "japanese", ()),
         "zero_echoes": ("zero_echoes", None, ("20000617", "2180")),
         "arcance": ("arcance", None, ())}
# A Memory Stick Pro Duo in a PSP-3000 reads about 1.5 to 2 MB/s, so a scene that
# loads under 1 second must stay well below this many bytes of images.
SCENE_BYTES_LIMIT = 1_500_000


def converter_stamp():
    h = hashlib.sha1()
    for path in sorted(glob.glob(os.path.join(REPO, "rpy2psp", "**", "*.py"), recursive=True)):
        with open(path, "rb") as f:
            h.update(f.read())
    return h.hexdigest()


def converted(name):
    """Compiles and packs a target game into the cache; returns (folder, inputs)."""
    rel, language, inputs = GAMES[name]
    src = os.path.join(TARGETS, rel)
    if not os.path.isdir(src):
        raise unittest.SkipTest(f"target game not present: {src}")
    out = os.path.join(CACHE, name)
    stamp = os.path.join(out, "converter.sha1")
    want = converter_stamp()
    if os.path.isfile(stamp):
        with open(stamp) as f:
            if f.read() == want:
                return out, inputs
    comp = compile_game(src, language)
    if comp.errors:
        raise AssertionError(f"{name} does not compile: {comp.errors[:3]}")
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "script.vnb"), "wb") as f:
        f.write(comp.bytecode())
    fonts = [os.path.join(TARGETS, "the_question", "renpy", "common"), os.path.join(TARGETS, "the_question", "sdk-fonts")]
    g = load_game(src)
    try:
        data, _ = build_pack(g, comp.manifest(), PackOptions(font_dirs=fonts))
    finally:
        g.close()
    with open(os.path.join(out, "data.pak"), "wb") as f:
        f.write(data)
    with open(stamp, "w") as f:
        f.write(want)
    return out, inputs


def field(lines, kind):
    return [ln.split("\t", 1)[1] for ln in lines if ln.startswith(kind + "\t")]


def script_for(choices, inputs, tail="play\n"):
    return ("choices " + " ".join(map(str, choices)) + "\n" if choices else "") + \
        ("inputs " + " ".join(inputs) + "\n" if inputs else "") + tail


def setUpModule():
    if not vnpsp_exe():
        raise unittest.SkipTest("vnpsp executable not built (set VNPSP_EXE)")


class TargetGamesTest(unittest.TestCase):
    maxDiff = None

    def check_paths(self, name, every=1, real_timing=1):
        game, inputs = converted(name)
        with open(os.path.join(game, "script.vnb"), "rb") as f:
            vnb = f.read()
        runs = explore(Program(vnb), max_depth=12, max_paths=4000, inputs=inputs)
        paths = [c for c, _, finished in runs if finished][::every]
        self.assertTrue(paths)
        scene_bytes = {}
        for i, choices in enumerate(paths):
            ref, finished = reference_trace(vnb, choices, inputs)
            self.assertTrue(finished)
            extra = [] if i < real_timing else ["--fast"]
            rc, lines = run_driver(game, script_for(choices, inputs), extra=extra)
            with self.subTest(game=name, choices=choices):
                self.assertEqual(lines[-1], "result\tok", lines[-5:])
                self.assertEqual(story_lines(lines), ref)
                self.assertEqual(field(lines, "missing-glyphs"), ["0"])
            for p in field(lines, "prefetch"):
                label, size = p.split("\t")
                scene_bytes[label] = max(scene_bytes.get(label, 0), int(size))
        return paths, scene_bytes

    def test_the_question(self):
        paths, scenes = self.check_paths("the_question")
        self.assertEqual(len(paths), 3)
        self.assertLess(max(scenes.values()), SCENE_BYTES_LIMIT)

    def test_the_question_japanese(self):
        self.check_paths("the_question_ja", real_timing=3)

    def test_zero_echoes(self):
        paths, scenes = self.check_paths("zero_echoes", every=4)
        self.assertGreater(len(paths), 20)
        self.assertLess(max(scenes.values()), SCENE_BYTES_LIMIT)

    def test_arcance(self):
        paths, scenes = self.check_paths("arcance", every=10)
        self.assertGreater(len(paths), 50)
        self.assertLess(max(scenes.values()), SCENE_BYTES_LIMIT)

    def test_arcance_rolls_back_55_steps(self):
        game, _ = converted("arcance")
        # Always the second choice, so the replay after the rollback picks the same ones.
        choices = [1] * 40
        forward = "start\n" + "advance 1\nstate\n" * 70
        rc, lines = run_driver(game, script_for(choices, (), forward), extra=["--fast"])
        self.assertEqual(rc, 0, lines[-5:])
        states = field(lines, "state")
        # Timed pauses are interactions but, as in Ren'Py, not rollback points: the
        # rollback steps are the distinct checkpoints.
        points = [s for i, s in enumerate(states) if i == 0 or s != states[i - 1]]
        self.assertEqual(len(points), len(set(points)))
        self.assertGreaterEqual(len(points), 60)
        target = points[-1 - 55]
        replay = len(states) - 1 - states.index(target)
        tail = forward + "hash\nrollback 55\nstate\nhash\n" + f"advance {replay}\nstate\nhash\n"
        rc, lines = run_driver(game, script_for(choices, (), tail), extra=["--fast"])
        self.assertEqual(rc, 0, lines[-5:])
        after, frames = field(lines, "state")[-2:], field(lines, "frame")
        self.assertEqual(after[0], target)               # 55 checkpoints back
        self.assertEqual(after[1], states[-1])           # and forward again to the same state
        self.assertEqual(frames[0], frames[2])

    def test_tiny_texture_cache_draws_the_same_frames(self):
        game, _ = converted("arcance")
        tail = "start\n" + "advance 7\nhash\n" * 8
        script = script_for([1] * 20, (), tail)
        rc, full = run_driver(game, script, extra=["--fast"])
        self.assertEqual(rc, 0, full[-5:])
        # 150 KB holds about one background: images are evicted and read again all the time.
        rc, tiny = run_driver(game, script, extra=["--fast", "--texture-budget", "150000"])
        self.assertEqual(rc, 0, tiny[-5:])
        self.assertEqual(field(tiny, "frame"), field(full, "frame"))
        self.assertEqual(story_lines(tiny), story_lines(full))
        loads_full, loads_tiny = int(field(full, "assets")[0].split("\t")[0]), int(field(tiny, "assets")[0].split("\t")[0])
        self.assertGreater(loads_tiny, loads_full)

    def test_save_and_load_every_game(self):
        for name in ("the_question", "zero_echoes", "arcance"):
            game, inputs = converted(name)
            saves = tempfile.mkdtemp(prefix="vnpsp-saves-")
            tail = "start\nadvance 9\nwait 20\nstate\nhash\nsave 12\nadvance 8\nload 12\nwait 20\nstate\nhash\n"
            choices = [0] * 12 if name != "arcance" else [1] * 12
            rc, lines = run_driver(game, script_for(choices, inputs, tail), save_dir=saves)
            with self.subTest(game=name):
                self.assertEqual(rc, 0, lines[-5:])
                states, frames = field(lines, "state"), field(lines, "frame")
                self.assertEqual(states[0], states[1])
                self.assertEqual(frames[0], frames[1])


if __name__ == "__main__":
    unittest.main()
