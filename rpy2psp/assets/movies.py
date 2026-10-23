"""Optional cutscene conversion (renpy.movie_cutscene) with an external ffmpeg.

Video becomes an H.264 Baseline Annex B elementary stream at 480x272 (letterboxed),
the format the PSP's AVC decoder takes; audio becomes MP3 through the music path.
No target game uses movies, and playback on hardware is phase 04 work.
"""

import json
import os
import shutil
import struct
import subprocess
import tempfile

from .audio import decode_source, encode_mp3
from .images import AssetError

VIDEO_ARGS = ["-vf", "scale=480:272:force_original_aspect_ratio=decrease,pad=480:272:(ow-iw)/2:(oh-ih)/2",
              "-c:v", "libx264", "-profile:v", "baseline", "-level", "3.0", "-pix_fmt", "yuv420p",
              "-b:v", "768k", "-r", "30000/1001", "-an", "-f", "h264"]


def find_ffmpeg():
    exe = shutil.which("ffmpeg")
    if exe is None:
        raise AssetError("movie conversion needs ffmpeg on PATH (https://ffmpeg.org)")
    return exe


def _run(args):
    r = subprocess.run(args, capture_output=True)
    if r.returncode != 0:
        raise AssetError(f"{os.path.basename(args[0])} failed: {r.stderr.decode(errors='replace')[-400:]}")
    return r.stdout


def convert_movie(data, name):
    """Return the MOV1 entry: header, H.264 stream, MP3 audio (may be empty)."""
    ffmpeg = find_ffmpeg()
    tmp = tempfile.mkdtemp(prefix="rpy2psp-movie-")
    try:
        src = os.path.join(tmp, "in" + os.path.splitext(name)[1])
        with open(src, "wb") as f:
            f.write(data)
        h264 = _run([ffmpeg, "-v", "error", "-y", "-i", src] + VIDEO_ARGS + ["-"])
        wav = _run([ffmpeg, "-v", "error", "-y", "-i", src, "-vn", "-ac", "2", "-ar", "44100", "-f", "wav", "-"])
        mp3 = b""
        if len(wav) > 44:
            frames, rate = decode_source(wav, name)
            if len(frames):
                mp3, _ = encode_mp3(frames, rate)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    header = b"MOV1" + struct.pack("<IIHHf", len(h264), len(mp3), 480, 272, 30000 / 1001)
    return header + h264 + mp3


def probe_movie(entry):
    """ffprobe the stored H.264 stream; returns codec, profile, width, height."""
    if entry[:4] != b"MOV1":
        raise AssetError("not a MOV1 entry")
    vlen = struct.unpack_from("<I", entry, 4)[0]
    ffprobe = shutil.which("ffprobe")
    if ffprobe is None:
        raise AssetError("ffprobe not found")
    tmp = tempfile.mkdtemp(prefix="rpy2psp-probe-")
    try:
        path = os.path.join(tmp, "v.264")
        with open(path, "wb") as f:
            f.write(entry[20:20 + vlen])
        out = _run([ffprobe, "-v", "error", "-show_streams", "-of", "json", path])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    s = json.loads(out)["streams"][0]
    return {"codec": s.get("codec_name"), "profile": s.get("profile"), "width": s.get("width"),
            "height": s.get("height")}
