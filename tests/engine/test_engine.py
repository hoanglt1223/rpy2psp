"""End-to-end tests of the vnpsp engine on PC, headless (no window, no audio device).

The self-written test game (make_test_game.py) is compiled, packed, and played by
the headless driver, which presses buttons frame by frame. Checks:

- every menu path traces the same story events as the reference interpreter;
- values shown in text match Python (expressions, formatting, random numbers);
- saving and loading, rollback, and replays restore identical states and frames;
- music loops without a gap; damaged saves are refused; persistent data survives.

Needs the vnpsp executable (built by CMake; set VNPSP_EXE to override) and a
system TrueType font for the test game.
"""

import os
import random
import shutil
import struct
import sys
import tempfile
import unittest

import numpy as np

from tests.compiler.refvm import Program, explore
from tests.engine import make_test_game
from tests.engine.harness import reference_trace, run_driver, story_lines, vnpsp_exe

INPUTS = ["Robin"]


def setUpModule():
    global GAME, TMP
    if not vnpsp_exe():
        raise unittest.SkipTest("vnpsp executable not built (set VNPSP_EXE)")
    try:
        make_test_game.system_font()
    except FileNotFoundError as e:
        raise unittest.SkipTest(str(e))
    TMP = tempfile.mkdtemp(prefix="vnpsp-tests-")
    GAME = os.path.join(TMP, "game")
    make_test_game.build(GAME)


def tearDownModule():
    shutil.rmtree(TMP, True)


def vnb():
    with open(os.path.join(GAME, "script.vnb"), "rb") as f:
        return f.read()


def finished_paths():
    runs = explore(Program(vnb()), max_depth=12, max_paths=200, inputs=INPUTS)
    return [choices for choices, _, finished in runs if finished]


def play_script(choices, inputs=INPUTS, tail="play\n"):
    return ("choices " + " ".join(map(str, choices)) + "\n" if choices else "") + \
        ("inputs " + " ".join(inputs) + "\n" if inputs else "") + tail


def field(lines, kind):
    return [ln.split("\t", 1)[1] for ln in lines if ln.startswith(kind + "\t")]


class ReferenceTest(unittest.TestCase):
    def test_every_path_matches_the_reference_interpreter(self):
        paths = finished_paths()
        self.assertGreaterEqual(len(paths), 3)
        for i, choices in enumerate(paths):
            ref, finished = reference_trace(vnb(), choices, INPUTS)
            self.assertTrue(finished)
            # Real timing (typing, pauses, transitions) on one path, skipping with X on the rest.
            extra = [] if i == 0 else ["--fast"]
            with self.subTest(choices=choices, fast=bool(extra)):
                rc, lines = run_driver(GAME, play_script(choices), extra=extra)
                self.assertEqual(lines[-1], "result\tok", lines[-5:])
                self.assertEqual(rc, 0)
                self.assertEqual(story_lines(lines), ref)

    def test_interpolated_values_match_python(self):
        rc, lines = run_driver(GAME, play_script(finished_paths()[0]), extra=["--fast"])
        self.assertEqual(rc, 0)
        text = field(lines, "text")
        rng = random.Random(0)
        roll, pick = rng.randint(1, 6), rng.choice(["a", "b", "c"])
        for expected in ("My name is ROBIN and visits are 000.", "Counting 3 times; visits now 3.",
                         "Back from the call, _return is 6.", f"Random roll {roll}, pick {pick}.",
                         "Escapes: {{braces}} and [brackets]], 100% sure.",
                         "«Temporary smile.{w=0.3} Then a pause.{w} Click to go on. And this part extends the line.»"):
            self.assertIn(expected, text)


# Expressions the compiler accepts, checked against Python's own repr / format.
EXPRESSIONS = [
    "7 // 2", "-7 // 2", "7 % 3", "-7 % 3", "7 % -3", "7 / 2", "1 / 3", "0.1 + 0.2", "1e16", "1e-5", "123456789.0",
    "2.5 * 4", "-0.0", "10 / 4 * 2", "1.5e300 * 1e10", "round(2.5)", "round(3.5)", "round(-2.5)", "round(2.675, 2)",
    "abs(-3)", "abs(-2.5)", "min(3, 1, 2)", "max([4, 9, 2])", "max('apple', 'pear')", "int('42')", "int(' -7 ')",
    "int(3.9)", "int(-3.9)", "float('1.5')", "str(1.0)", "str(10)", "bool(0)", "bool('x')", "len('héllo')",
    "'héllo'[1]", "'abcdef'[1:4]", "'abcdef'[-2:]", "'abcdef'[:-2]", "'a,b,,c'.split(',')", "' x  y '.split()",
    "'a b c'.split(' ', 1)", "'-'.join(['a', 'b'])", "'Hello'.upper()", "'Hello'.lower()", "'hello world'.title()",
    "'hELLO'.capitalize()", "'  pad  '.strip()", "'xxhixx'.strip('x')", "'banana'.count('a')", "'banana'.index('n')",
    "'banana'.replace('a', 'o')", "'banana'.replace('a', 'o', 1)", "'abc'.startswith('ab')",
    "'abc'.endswith(('x', 'c'))", "[1, 2] + [3]", "(1, 2) + (3,)", "[0] * 3", "'ab' * 3", "3 * 'x'", "(1,)", "()",
    "[]", "{'a': 1, 'b': [2, 3]}", "{1: 'x'}.get(1)", "{1: 'x'}.get(2, 'd')", "list('abc')", "tuple([1, 2])",
    "len({'a': 1})", "list({'a': 1, 'b': 2}.keys())", "list({'a': 1}.items())", "3 in [1, 2, 3]", "'b' in 'abc'",
    "'k' in {'k': 1}", "4 not in (1, 2)", "None is None", "1 == 1.0", "True + 1", "[1, 2] == [1, 2]",
    "(1, 2) < (1, 3)", "'abc' < 'abd'", "not []", "0 or 'fallback'", "1 and 2", "'%s-%d' % ('a', 3)",
    "'%05.1f' % 3.14159", "'%s' % [1, 2]", "'%d%%' % 50", "'%r' % 'q'", "str(1 / 3)", "[1, 'two', 3.0, None, True]",
    "{'k': (1,)}['k']", "len([1, [2, 3]])", "1 if [] else 2", "'x'.join('abc')",
]
FORMATS = [("42", ">6"), ("42", "<6"), ("42", "^7"), ("3.14159", ".3f"), ("1234567", ","), ("3.14159", "e"),
           ("'hello'", ".2"), ("5", "+d"), ("3.14159", "08.2f"), ("0.256", ".1%"), ("255", "x"), ("5", "b"),
           ("1234567.891", ",.2f"), ("'ab'", "*^6")]


def parity_script():
    lines = ["define config.screen_width = 1280", "define config.screen_height = 720",
             "define gui.text_font = \"body.ttf\"", "label start:"]
    for i, ex in enumerate(EXPRESSIONS):
        lines.append(f"    $ r{i} = {ex}")
        lines.append(f"    \"v{i}=[r{i}!r]\"")
    for i, (val, spec) in enumerate(FORMATS):
        lines.append(f"    $ f{i} = {val}")
        lines.append(f"    \"f{i}=[f{i}:{spec}]\"")
    lines.append("    return")
    return "\n".join(lines) + "\n"


class ValueParityTest(unittest.TestCase):
    def test_values_and_formats_match_python(self):
        game = os.path.join(TMP, "parity")
        make_test_game.build(game, parity_script())
        rc, lines = run_driver(game, "play\n", extra=["--fast"])
        self.assertEqual(rc, 0, lines[-5:])
        got = field(lines, "text")
        expected = [f"v{i}={eval(ex)!r}" for i, ex in enumerate(EXPRESSIONS)]
        expected += [f"f{i}={format(eval(val), spec)}" for i, (val, spec) in enumerate(FORMATS)]
        # Text tags are not involved, but a Python repr may contain '[': escape it as the engine trace does.
        expected = [x.replace("\\", "\\\\") for x in expected]
        self.maxDiff = None
        self.assertEqual(got, expected)


class StateTest(unittest.TestCase):
    def test_save_and_load_restore_state_and_frame(self):
        saves = os.path.join(TMP, "saves-saveload")
        script = play_script(finished_paths()[0], tail="start\nadvance 6\nwait 30\nstate\nhash\nsave 7\n"
                                                         "advance 6\nstate\nload 7\nwait 30\nstate\nhash\n")
        rc, lines = run_driver(GAME, script, save_dir=saves)
        self.assertEqual(rc, 0, lines[-5:])
        states, frames = field(lines, "state"), field(lines, "frame")
        self.assertEqual(states[0], states[2])
        self.assertNotEqual(states[0], states[1])
        self.assertEqual(frames[0], frames[1])
        self.assertTrue(os.path.isfile(os.path.join(saves, "slot-07.sav")))

    def test_rollback_returns_to_earlier_states(self):
        # Twelve lines forward (through the name input), ten back, ten forward again. The
        # replay types the name a second time, so the driver gets it twice.
        script = play_script(finished_paths()[0], inputs=INPUTS * 2,
                             tail="start\n" + "advance 1\nstate\n" * 12 + "rollback 10\nstate\nadvance 10\nstate\n")
        rc, lines = run_driver(GAME, script)
        self.assertEqual(rc, 0, lines[-5:])
        states = field(lines, "state")
        self.assertEqual(len(set(states[:12])), 12)
        self.assertEqual(states[12], states[1])
        self.assertEqual(states[13], states[11])

    def test_runs_are_deterministic(self):
        script = play_script(finished_paths()[0], tail="start\nadvance 5\nhash\nadvance 5\nhash\nplay\n")
        a = run_driver(GAME, script, extra=["--render-all", "--audio"])
        b = run_driver(GAME, script, extra=["--render-all", "--audio"])
        self.assertEqual(a[0], 0, a[1][-5:])
        self.assertEqual(a, b)
        self.assertEqual(len(field(a[1], "frame")), 2)
        audio = field(a[1], "audio")[0].split("\t")
        self.assertGreater(int(audio[1]), 0)   # music and effects were mixed

    def test_damaged_save_is_refused(self):
        saves = os.path.join(TMP, "saves-damaged")
        script = play_script(finished_paths()[0], tail="start\nadvance 3\nsave 2\n")
        self.assertEqual(run_driver(GAME, script, save_dir=saves)[0], 0)
        path = os.path.join(saves, "slot-02.sav")
        with open(path, "r+b") as f:
            f.seek(200)
            b = f.read(1)
            f.seek(200)
            f.write(bytes([b[0] ^ 0xFF]))
        script = play_script(finished_paths()[0], tail="start\nadvance 2\nstate\npress START\npress DOWN 2\npress X\n"
                                                         "press RIGHT\npress X\nexpect load\npress O\npress O\nstate\n")
        rc, lines = run_driver(GAME, script, save_dir=saves)
        self.assertEqual(rc, 0, lines[-5:])
        self.assertIn("This save is damaged or from a different build of the game.", field(lines, "message"))
        states = field(lines, "state")
        self.assertEqual(states[0], states[1])

    def test_persistent_data_survives_a_restart(self):
        saves = os.path.join(TMP, "saves-persistent")
        script = play_script(finished_paths()[0])
        notes = []
        for _ in range(2):
            rc, lines = run_driver(GAME, script, save_dir=saves, extra=["--fast"])
            self.assertEqual(rc, 0, lines[-5:])
            notes += [x for x in field(lines, "call") if x.startswith("renpy.notify")]
        self.assertEqual(notes[0], "renpy.notify\t['Saved runs: 1']")
        self.assertEqual(notes[-1], "renpy.notify\t['Saved runs: 2']")


class AudioTest(unittest.TestCase):
    def test_music_loops_without_a_gap(self):
        # Start, then idle on the first line long enough for the 2 s track to loop twice.
        wav = os.path.join(TMP, "mix.wav")
        script = play_script(finished_paths()[0], tail="start\nwait 330\n")
        rc, lines = run_driver(GAME, script, extra=["--wav", wav])
        self.assertEqual(rc, 0, lines[-5:])
        with open(wav, "rb") as f:
            data = f.read()
        self.assertEqual(data[:4], b"RIFF")
        pcm = np.frombuffer(data[44:], dtype=np.int16).reshape(-1, 2)[:, 0].astype(np.float64)
        start = int(np.argmax(np.abs(pcm) > 1000))
        n = make_test_game.MUSIC_SECONDS * 44100
        self.assertGreater(len(pcm) - start, 2 * n + 4410)
        # A trimmed, gapless loop repeats the first pass sample for sample...
        np.testing.assert_array_equal(pcm[start:start + n], pcm[start + n:start + 2 * n])
        # ...and never drops out: every 5 ms window keeps the tone's level.
        body = pcm[start:start + 2 * n + 2205]
        win = 220
        rms = np.sqrt(np.mean(body[:len(body) // win * win].reshape(-1, win) ** 2, axis=1))
        self.assertGreater(rms.min(), 0.5 * np.median(rms))
        # The tone keeps its phase across the loop point (period 100 samples).
        seam = pcm[start + n - 300:start + n + 300]
        ref = np.sin(2 * np.pi * np.arange(600) / 100.0)
        corr = max(np.dot(seam, np.roll(ref, k)) for k in range(100)) / (np.linalg.norm(seam) * np.linalg.norm(ref))
        self.assertGreater(corr, 0.95)


if __name__ == "__main__":
    unittest.main()
