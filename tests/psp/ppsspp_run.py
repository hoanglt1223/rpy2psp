"""Runs a built game (`rpy2psp build` output) in PPSSPPHeadless and in the PC
engine with the same timed button presses, then compares the screenshots and
reports the engine's timing log lines and the save files.

    python tests/psp/ppsspp_run.py <game folder> <timeline> -o <out dir>
        [--vnpsp <PC vnpsp exe>] [--ppsspp "docker run ... ppsspp-headless:local PPSSPPHeadless"]

Timeline lines (times in emulated seconds; '#' starts a comment):
    4.0 press X [0.1]      press a button (UP DOWN LEFT RIGHT X O SQUARE TRIANGLE L R START SELECT)
    8.0 shot dialogue      screenshot named dialogue.png
    60  end                stop here

The PC run starts counting when the engine starts, PPSSPP when the EBOOT boots,
so a timeline should only take shots on settled screens (text fully typed, no
transition running). Shots are compared by PSNR; GE and software rendering
round differently, so they are not expected to be bit-identical.
Writes <out>/report.json and prints a summary; exit code 1 when a check fails.
"""

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image

BUTTONS = {"SELECT": 0x1, "START": 0x8, "UP": 0x10, "RIGHT": 0x20, "DOWN": 0x40, "LEFT": 0x80, "L": 0x100,
           "R": 0x200, "TRIANGLE": 0x1000, "O": 0x2000, "X": 0x4000, "SQUARE": 0x8000}
FPS = 60
DEFAULT_PPSSPP = ("docker run --rm --user {uid} -e HOME=/tmp -v {out}:{out} -w {cwd} "
                  "ppsspp-headless:local PPSSPPHeadless")
PSNR_MIN = 30.0


def parse_timeline(path):
    events, end = [], None
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            parts = line.split("#", 1)[0].split()
            if not parts:
                continue
            try:
                t = float(parts[0])
            except ValueError:
                raise SystemExit(f"{path}:{n}: bad time {parts[0]!r}") from None
            kind = parts[1] if len(parts) > 1 else ""
            if kind == "press" and len(parts) in (3, 4) and parts[2] in BUTTONS:
                events.append((t, "press", parts[2], float(parts[3]) if len(parts) == 4 else 0.1))
            elif kind == "shot" and len(parts) == 3 and re.fullmatch(r"[\w-]+", parts[2]):
                events.append((t, "shot", parts[2], 0))
            elif kind == "end" and len(parts) == 2:
                end = t
            else:
                raise SystemExit(f"{path}:{n}: cannot parse {line.strip()!r}")
    if end is None:
        raise SystemExit(f"{path}: no 'end' line")
    return sorted(events, key=lambda e: e[0]), end


def driver_script(events, end):
    """The same presses for the PC driver, frame by frame."""
    lines, frame = [], 0
    for t, kind, arg, dur in events:
        at = round(t * FPS)
        if at > frame:
            lines.append(f"wait {at - frame}")
            frame = at
        if kind == "press":
            hold = max(1, round(dur * FPS))
            lines.append(f"hold {arg} {hold}")
            frame += hold + 1
        else:
            lines.append(f"shot {{out}}/pc-{arg}.png")
    if round(end * FPS) > frame:
        lines.append(f"wait {round(end * FPS) - frame}")
    return "\n".join(lines) + "\n"


def psnr(a_path, b_path):
    # PPSSPP saves the 512-pixel frame buffer stride; the screen is its left 480 columns.
    with Image.open(a_path) as a, Image.open(b_path) as b:
        a = np.asarray(a.convert("RGB").crop((0, 0, 480, 272)), dtype=np.float64)
        b = np.asarray(b.convert("RGB").crop((0, 0, 480, 272)), dtype=np.float64)
    mse = float(np.mean((a - b) ** 2))
    return 99.0 if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("game", help="folder made by `rpy2psp build` (EBOOT.PBP, script.vnb, data.pak)")
    ap.add_argument("timeline")
    ap.add_argument("-o", "--out", required=True, help="scratch folder: memory stick, shots, logs")
    ap.add_argument("--vnpsp", help="PC engine for the comparison run (skipped when absent)")
    ap.add_argument("--ppsspp", default=DEFAULT_PPSSPP, help="command prefix; {out}, {cwd} and {uid} are filled in")
    args = ap.parse_args()

    events, end = parse_timeline(args.timeline)
    out = os.path.abspath(args.out)
    folder = os.path.basename(os.path.abspath(args.game))
    ms = os.path.join(out, "ms")
    shutil.rmtree(ms, ignore_errors=True)
    shutil.copytree(args.game, os.path.join(ms, "PSP", "GAME", folder))
    shots = os.path.join(out, "shots")
    shutil.rmtree(shots, ignore_errors=True)
    os.makedirs(shots)

    presses = ",".join(f"{t:g}:{BUTTONS[a]:#x}:{d:g}" for t, k, a, d in events if k == "press")
    shot_at = ",".join(f"{t:g}:psp-{a}.png" for t, k, a, _ in events if k == "shot")
    uid = f"{os.getuid()}:{os.getgid()}" if hasattr(os, "getuid") else "0:0"
    cmd = shlex.split(args.ppsspp.format(out=out, cwd=shots, uid=uid)) + [
        f"--memstick={ms}", f"--timeout-emulated={end:g}", f"--screenshot-at={shot_at}", f"--press={presses}",
        os.path.join(ms, "PSP", "GAME", folder, "EBOOT.PBP")]
    print("ppsspp:", " ".join(cmd), flush=True)
    run = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=1800)
    log = run.stdout + run.stderr
    with open(os.path.join(out, "ppsspp.log"), "w", encoding="utf-8") as f:
        f.write(log)

    report = {"ppsspp_exit": run.returncode, "failures": []}
    fail = report["failures"]
    report["log"] = [ln.strip() for ln in log.splitlines() if "vnpsp" in ln or "plat:" in ln][:200]
    for key, pattern in (("started_ms", r"vnpsp: started in (\d+) ms"),
                         ("scene_loads", r"vnpsp: scene (\S+): (\d+) bytes in (\d+) ms"),
                         ("perf", r"vnpsp: perf: (.*)")):
        report[key] = re.findall(pattern, log)
    if not report["started_ms"]:
        fail.append("the engine never logged its start")
    for label, size, ms_ in report["scene_loads"]:
        if int(ms_) >= 1000:
            fail.append(f"scene {label} took {ms_} ms to load")
    if re.search(r"vnpsp: (?!started|scene|perf)", log):
        report["engine_messages"] = re.findall(r"vnpsp: (?!started|scene|perf)(.*)", log)

    saves = os.path.join(ms, "PSP", "SAVEDATA", re.sub(r"[^A-Za-z0-9_-]", "", folder) or "VNPSP")
    report["savedata"] = sorted(os.listdir(saves)) if os.path.isdir(saves) else []

    missing = [a for _, k, a, _ in events if k == "shot" and not os.path.isfile(os.path.join(shots, f"psp-{a}.png"))]
    if missing:
        fail.append(f"PPSSPP wrote no screenshot for {missing}")

    if args.vnpsp:
        script = os.path.join(out, "pc-script.txt")
        with open(script, "w", encoding="utf-8") as f:
            f.write(driver_script(events, end).replace("{out}", shots))
        pc_saves = os.path.join(out, "pc-saves")
        shutil.rmtree(pc_saves, ignore_errors=True)
        pc = subprocess.run([args.vnpsp, args.game, "--play", script, "--trace", os.path.join(out, "pc-trace.txt"),
                             "--save-dir", pc_saves], capture_output=True, text=True, timeout=600)
        if pc.returncode != 0:
            fail.append(f"PC run failed ({pc.returncode}): {pc.stderr.strip()[-300:]}")
        report["pc_savedata"] = sorted(os.listdir(pc_saves)) if os.path.isdir(pc_saves) else []
        report["psnr"] = {}
        for _, k, a, _ in events:
            p, c = os.path.join(shots, f"psp-{a}.png"), os.path.join(shots, f"pc-{a}.png")
            if k == "shot" and os.path.isfile(p) and os.path.isfile(c):
                report["psnr"][a] = round(psnr(p, c), 1)
                if report["psnr"][a] < PSNR_MIN:
                    fail.append(f"shot {a}: PSP and PC differ (PSNR {report['psnr'][a]} dB)")

    with open(os.path.join(out, "report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1)
    print(json.dumps({k: v for k, v in report.items() if k != "log"}, indent=1))
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
