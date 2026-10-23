"""Audio conversion: music to MP3, effects and voice to PCM or IMA ADPCM.

Sources (OGG Vorbis, Opus, MP3, WAV, FLAC) are decoded with libsndfile through
soundfile, resampled with soxr, and encoded:

- music: MPEG-1 Layer III, 44.1 kHz, 128 kbps CBR (lameenc) behind a MUS1 header that
  records the length and encoder delay, so the engine can loop without a gap;
- sound effects and voice: 16-bit PCM when shorter than `pcm_limit` seconds, else
  4-bit IMA ADPCM in independent blocks (layout in docs/dev/pack-format.md).
"""

import io
import struct

import lameenc
import numpy as np
import soundfile
import soxr

from .images import AssetError

PSP_RATES = (11025, 22050, 44100, 48000)
ADPCM_BLOCK = 1024  # frames per block

STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
         97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
         724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
         4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
         18500, 20350, 22385, 24623, 27086, 29794, 32767]
INDEX_ADJ = [-1, -1, -1, -1, 2, 4, 6, 8]


def decode_source(data, name):
    """Decode audio bytes to (float32 frames x channels, sample rate)."""
    try:
        frames, rate = soundfile.read(io.BytesIO(data), dtype="float32", always_2d=True)
    except (RuntimeError, soundfile.LibsndfileError) as e:
        raise AssetError(f"cannot decode audio {name!r}: {e}") from None
    if frames.shape[1] > 2:
        frames = frames[:, :2]
    return frames, rate


def resample(frames, rate, target):
    if rate == target:
        return frames
    return soxr.resample(frames, rate, target).astype(np.float32)


def to_int16(frames):
    return np.clip(np.round(frames * 32767.0), -32768, 32767).astype(np.int16)


# ---- MP3 -----------------------------------------------------------------------------------

def encode_mp3(frames, rate, bitrate=128):
    pcm = to_int16(resample(frames, rate, 44100))
    enc = lameenc.Encoder()
    enc.set_bit_rate(bitrate)
    enc.set_in_sample_rate(44100)
    enc.set_out_sample_rate(44100)
    enc.set_channels(pcm.shape[1])
    enc.set_quality(2)
    data = enc.encode(np.ascontiguousarray(pcm).tobytes()) + enc.flush()
    return bytes(data), pcm


# LAME's encoder delay (576) plus the standard MP3 decoder delay (529): decoded output
# starts this many frames before the first source sample.
MP3_DELAY = 1105
MUSIC_HEADER = struct.Struct("<4sIHBBI")


def music_entry(mp3, frames, channels):
    """MUS1 entry: header, then the MP3 stream (layout in docs/dev/pack-format.md)."""
    return MUSIC_HEADER.pack(b"MUS1", frames, MP3_DELAY, channels, 0, 44100) + mp3


def read_music_entry(blob):
    """Return (mp3 bytes, source frames, delay, channels)."""
    if blob[:4] != b"MUS1":
        raise AssetError("not a MUS1 entry")
    _, frames, delay, chans, _, rate = MUSIC_HEADER.unpack_from(blob)
    if rate != 44100:
        raise AssetError(f"MUS1 sample rate {rate} is not 44100")
    return blob[MUSIC_HEADER.size:], frames, delay, chans


def check_mp3_header(data):
    """Parse the first frame header; return (version, layer, bitrate kbps, rate)."""
    i = 0
    if data[:3] == b"ID3":
        size = (data[6] << 21) | (data[7] << 14) | (data[8] << 7) | data[9]
        i = 10 + size
    while i + 4 <= len(data):
        if data[i] == 0xFF and data[i + 1] & 0xE0 == 0xE0:
            b1, b2 = data[i + 1], data[i + 2]
            version = {3: 1, 2: 2, 0: 2.5}.get((b1 >> 3) & 3)
            layer = 4 - ((b1 >> 1) & 3)
            br = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0][b2 >> 4]
            sr = [44100, 48000, 32000, 0][(b2 >> 2) & 3]
            return version, layer, br, sr
        i += 1
    raise AssetError("no MP3 frame found")


# ---- IMA ADPCM -------------------------------------------------------------------------------

def _encode_channel(samples, pred, index):
    out = bytearray((len(samples) + 1) // 2)
    steps = STEPS
    adj = INDEX_ADJ
    for n, s in enumerate(samples.tolist()):
        step = steps[index]
        diff = s - pred
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        delta = step >> 3
        if diff >= step:
            code |= 4
            diff -= step
            delta += step
        if diff >= step >> 1:
            code |= 2
            diff -= step >> 1
            delta += step >> 1
        if diff >= step >> 2:
            code |= 1
            delta += step >> 2
        pred = pred - delta if code & 8 else pred + delta
        pred = -32768 if pred < -32768 else 32767 if pred > 32767 else pred
        index += adj[code & 7]
        index = 0 if index < 0 else 88 if index > 88 else index
        if n & 1:
            out[n >> 1] |= code << 4
        else:
            out[n >> 1] = code
    return bytes(out), pred, index


def _decode_channel(data, count, pred, index):
    out = np.empty(count, np.int16)
    steps = STEPS
    adj = INDEX_ADJ
    for n in range(count):
        code = (data[n >> 1] >> 4) if n & 1 else (data[n >> 1] & 15)
        step = steps[index]
        delta = step >> 3
        if code & 4:
            delta += step
        if code & 2:
            delta += step >> 1
        if code & 1:
            delta += step >> 2
        pred = pred - delta if code & 8 else pred + delta
        pred = -32768 if pred < -32768 else 32767 if pred > 32767 else pred
        index += adj[code & 7]
        index = 0 if index < 0 else 88 if index > 88 else index
        out[n] = pred
    return out, pred, index


def encode_adpcm(pcm):
    """pcm: int16 frames x channels -> block data. Each block: per channel
    (i16 predictor, u8 step index, u8 0, ADPCM_BLOCK/2 bytes of nibbles, low nibble first)."""
    frames, chans = pcm.shape
    state = [(0, 0)] * chans
    out = bytearray()
    for start in range(0, frames, ADPCM_BLOCK):
        block = pcm[start:start + ADPCM_BLOCK]
        if len(block) < ADPCM_BLOCK:
            block = np.pad(block, ((0, ADPCM_BLOCK - len(block)), (0, 0)))
        for c in range(chans):
            pred, index = state[c]
            out += struct.pack("<hBB", pred, index, 0)
            data, pred, index = _encode_channel(block[:, c], pred, index)
            out += data
            state[c] = (pred, index)
    return bytes(out)


def decode_adpcm(data, frames, chans):
    block_bytes = 4 + ADPCM_BLOCK // 2
    out = np.zeros((frames, chans), np.int16)
    pos = 0
    for start in range(0, frames, ADPCM_BLOCK):
        n = min(ADPCM_BLOCK, frames - start)
        for c in range(chans):
            pred, index, _ = struct.unpack_from("<hBB", data, pos)
            samples, _, _ = _decode_channel(data[pos + 4:pos + block_bytes], ADPCM_BLOCK, pred, index)
            out[start:start + n, c] = samples[:n]
            pos += block_bytes
    return out


# ---- effects container --------------------------------------------------------------------------

CODEC_PCM16 = 0
CODEC_IMA_ADPCM = 1


def convert_effect(frames, rate, pcm_limit=1.0):
    """Return (header + payload bytes, reference int16 pcm, codec)."""
    target = rate if rate in PSP_RATES else 44100
    pcm = to_int16(resample(frames, rate, target))
    n, chans = pcm.shape
    if n / target <= pcm_limit:
        codec, payload = CODEC_PCM16, np.ascontiguousarray(pcm).tobytes()
    else:
        codec, payload = CODEC_IMA_ADPCM, encode_adpcm(pcm)
    header = b"SND1" + struct.pack("<IBBHI", target, chans, codec, ADPCM_BLOCK, n)
    return header + payload, pcm, codec


def decode_effect(blob):
    if blob[:4] != b"SND1":
        raise AssetError("not an SND1 entry")
    rate, chans, codec, _, n = struct.unpack_from("<IBBHI", blob, 4)
    body = blob[16:]
    if codec == CODEC_PCM16:
        return np.frombuffer(body, np.int16).reshape(n, chans), rate
    return decode_adpcm(body, n, chans), rate


def snr_db(reference, test):
    ref = reference.astype(np.float64)
    t = test.astype(np.float64)
    n = min(len(ref), len(t))
    noise = ((ref[:n] - t[:n]) ** 2).sum()
    sig = (ref[:n] ** 2).sum()
    if noise == 0:
        return float("inf")
    if sig == 0:
        return float("-inf")
    return 10 * np.log10(sig / noise)


def verify_mp3(data, reference_pcm):
    """Decode the MP3 back, align it to the reference (encoder/decoder delay), and measure it."""
    dec, rate = soundfile.read(io.BytesIO(data), dtype="int16", always_2d=True)
    ref = reference_pcm[:, 0].astype(np.float64)
    got = dec[:, 0].astype(np.float64)
    window = min(len(ref), 44100 * 5)
    best, best_lag = -1.0, 0
    for lag in range(0, 4000):
        seg = got[lag:lag + window]
        if len(seg) < window:
            break
        c = float(np.dot(seg, ref[:window]))
        if c > best:
            best, best_lag = c, lag
    aligned = dec[best_lag:best_lag + len(reference_pcm)]
    return {"rate": rate, "channels": dec.shape[1], "lag": best_lag,
            "duration_diff": (len(dec) - best_lag - len(reference_pcm)) / 44100.0,
            "snr": float(snr_db(reference_pcm[:len(aligned)], aligned))}
