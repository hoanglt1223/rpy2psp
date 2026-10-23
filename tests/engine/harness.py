"""Helpers for the engine tests: run the vnpsp driver, and format reference-interpreter
events the way the engine traces them, so whole playthroughs can be compared line by line."""

import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "compiler"))

from refvm import VM, NeedChoice, Program  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_EXE = [os.path.join(ROOT, "out", "msvc", "vnpsp", cfg, "vnpsp.exe") for cfg in ("Release", "Debug")]
# The trace lines that come from the engine's interpretation of the script (the rest
# are driver bookkeeping such as frame hashes).
STORY_KINDS = {"say", "attrs", "menu", "pause", "with", "show", "scene", "hide", "play", "voice", "movie", "stop",
               "window", "nvl", "voice_sustain", "input", "call", "end"}


def vnpsp_exe():
    exe = os.environ.get("VNPSP_EXE")
    if exe:
        return exe
    for path in DEFAULT_EXE:
        if os.path.isfile(path):
            return path
    return shutil.which("vnpsp")


def _esc(s):
    return "None" if s is None else str(s).replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")


def format_event(ev):
    k = ev[0]
    if k == "say":
        return f"say\t{_esc(ev[1])}\t{_esc(ev[2])}"
    if k == "attrs":
        return f"attrs\t{_esc(ev[1])}\t{' '.join(ev[2])}"
    if k == "menu":
        return "menu\t" + "\t".join(_esc(x) for x in ev[1]) + "\t-> " + _esc(ev[2])
    if k == "pause":
        return f"pause\t{ev[1]!r}"
    if k == "with":
        return f"with\t{ev[1]}"
    if k in ("show", "scene", "hide"):
        rec = ev[1]
        if rec is None:
            return f"{k}\tNone"
        return f"{k}\t{_esc(rec[0])}\t{' '.join(rec[1])}"
    if k in ("play", "voice", "movie", "stop"):
        return f"{k}\t{ev[1]}"
    if k in ("window", "nvl"):
        return f"{k}\t{ev[1]}\t{ev[2]}"
    if k == "voice_sustain":
        return k
    if k == "input":
        return f"input\t{_esc(ev[1])}\t{_esc(ev[2])}"
    if k == "call":
        return f"call\t{ev[1]}\t{_esc(repr(ev[2]))}"
    if k == "end":
        return "end"
    raise ValueError(f"unknown event {ev!r}")


def reference_trace(vnb_bytes, choices, inputs=()):
    """Plays the script in the reference interpreter; returns (trace lines, finished)."""
    vm = VM(Program(vnb_bytes), choices=list(choices), inputs=list(inputs))
    try:
        events = vm.play()
        finished = True
    except NeedChoice:
        events = vm.events
        finished = False
    return [format_event(e) for e in events], finished


def run_driver(game_dir, script, save_dir=None, extra=(), timeout=600):
    """Runs the headless driver; returns (return code, trace lines)."""
    exe = vnpsp_exe()
    if not exe:
        raise FileNotFoundError("vnpsp executable not found; build it or set VNPSP_EXE")
    tmp = tempfile.mkdtemp(prefix="vnpsp-run-")
    try:
        sp = os.path.join(tmp, "script.txt")
        with open(sp, "w", encoding="utf-8") as f:
            f.write(script)
        tp = os.path.join(tmp, "trace.txt")
        cmd = [exe, game_dir, "--play", sp, "--trace", tp, "--save-dir", save_dir or os.path.join(tmp, "saves")]
        proc = subprocess.run(cmd + list(extra), capture_output=True, timeout=timeout)
        if not os.path.isfile(tp):
            raise AssertionError(f"vnpsp wrote no trace (exit code {proc.returncode}): "
                                 f"{proc.stderr.decode('utf-8', 'replace')[-2000:]}")
        # Corrupt inputs can put invalid UTF-8 into the trace.
        with open(tp, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
        return proc.returncode, lines
    finally:
        shutil.rmtree(tmp, True)


def story_lines(lines):
    return [ln for ln in lines if ln.split("\t", 1)[0] in STORY_KINDS]
