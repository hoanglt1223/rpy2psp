"""Checks that music loops without a gap on the PSP build of the engine, in PPSSPPHeadless.

    python tests/psp/music_loop_check.py -o <scratch dir> [--engine out/psp/vnpsp/EBOOT.PBP] [--vnpsp <PC exe>]

Builds a self-made game whose music is a 2 s, 441 Hz sine (exactly 100 samples
per period at 44.1 kHz, so a seamless loop is periodic across the seam), runs it
for several loops with PPSSPP's audio dump, and measures how far the recording
departs from that period. A gap or a click at a loop shows up as a large
deviation; MP3 coding alone leaves about 5% at each seam. With --vnpsp, the PC
engine mixes the same game and the two recordings must match sample for sample
(the PC tests check that loop for exact length and phase). Exit code 1 on failure.
"""

import argparse
import os
import shutil
import subprocess
import sys

import numpy as np
import soundfile

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from rpy2psp.eboot import make_eboot  # noqa: E402
from tests.engine.make_test_game import MUSIC_HZ, MUSIC_SECONDS, build  # noqa: E402

SCRIPT = '''
define gui.text_font = "body.ttf"
label start:
    play music "audio/theme.ogg"
    "Listening to the loop."
    return
'''
PERIOD = 44100 // MUSIC_HZ
SECONDS = 4 + MUSIC_SECONDS * 5
DEFAULT_PPSSPP = "docker run --rm --user {uid} -e HOME=/tmp -v {out}:{out} -w {out} ppsspp-headless:local PPSSPPHeadless"


def main():
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--engine", default=os.path.join(root, "out", "psp", "vnpsp", "EBOOT.PBP"))
    ap.add_argument("--ppsspp", default=DEFAULT_PPSSPP)
    ap.add_argument("--vnpsp", help="PC engine: also compare the PSP recording with its mix")
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    game = os.path.join(out, "ms", "PSP", "GAME", "LoopCheck")
    shutil.rmtree(os.path.join(out, "ms"), ignore_errors=True)
    build(game, SCRIPT)
    with open(args.engine, "rb") as f:
        engine = f.read()
    with open(os.path.join(game, "EBOOT.PBP"), "wb") as f:
        f.write(make_eboot(engine, "Loop check"))
    wav = os.path.join(out, "loop.wav")
    if os.path.exists(wav):
        os.remove(wav)
    uid = f"{os.getuid()}:{os.getgid()}" if hasattr(os, "getuid") else "0:0"
    cmd = args.ppsspp.format(out=out, uid=uid).split() + [
        f"--memstick={os.path.join(out, 'ms')}", f"--timeout-emulated={SECONDS}", "--press=3:0x4000:0.1",
        f"--dump-audio={wav}", os.path.join(game, "EBOOT.PBP")]
    run = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=900)
    if not os.path.isfile(wav):
        print(run.stdout[-2000:], run.stderr[-2000:])
        print("FAIL: PPSSPP wrote no audio dump")
        return 1
    data, rate = soundfile.read(wav, always_2d=True)
    left = data[:, 0]
    loud = np.flatnonzero(np.abs(left) > 0.05)
    if rate != 44100 or len(loud) == 0:
        print(f"FAIL: rate {rate}, {len(loud)} audible samples")
        return 1
    # Skip the start of playback and the fade-in; keep at least three loop seams.
    start = loud[0] + rate // 2
    seg = left[start:loud[-1] - rate // 10]
    seams = len(seg) / (rate * MUSIC_SECONDS)
    amp = np.percentile(np.abs(seg), 99.5)
    dev = np.abs(seg[PERIOD:] - seg[:-PERIOD])
    worst = float(dev.max() / amp) if amp else 1.0
    print(f"recorded {len(left) / rate:.1f} s, analysed {len(seg) / rate:.1f} s (~{seams:.1f} loops), "
          f"amplitude {amp:.3f}, worst period deviation {worst * 100:.2f}% of amplitude")
    if seams < 3:
        print("FAIL: fewer than three loops recorded")
        return 1
    if worst > 0.10:
        print("FAIL: the music is not periodic across loops (gap or click)")
        return 1
    if args.vnpsp:
        script = os.path.join(out, "pc-script.txt")
        with open(script, "w") as f:
            f.write(f"wait 180\nhold X 6\nwait {SECONDS * 60 - 187}\n")
        pc_wav = os.path.join(out, "pc.wav")
        subprocess.run([args.vnpsp, game, "--play", script, "--trace", os.path.join(out, "pc-trace.txt"),
                        "--wav", pc_wav, "--save-dir", os.path.join(out, "pc-saves")], check=True, timeout=300)
        pc, _ = soundfile.read(pc_wav, always_2d=True)
        pc = pc[:, 0]
        pc_start = np.flatnonzero(np.abs(pc) > 0.05)[0]
        n = min(len(left) - loud[0], len(pc) - pc_start) - rate // 10
        diff = float(np.max(np.abs(left[loud[0]:loud[0] + n] - pc[pc_start:pc_start + n])))
        print(f"PSP vs PC mix over {n / rate:.1f} s: largest sample difference {diff * 32768:.0f} LSB")
        if diff * 32768 > 2:
            print("FAIL: the PSP mix differs from the PC engine's")
            return 1
    print("ok: gapless")
    return 0


if __name__ == "__main__":
    sys.exit(main())
