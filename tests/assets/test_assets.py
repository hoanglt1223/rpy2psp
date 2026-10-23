"""Asset pipeline tests on generated inputs (no game data)."""

import io
import os
import shutil
import struct
import subprocess
import tempfile
import unittest

import etcpak
import numpy as np
import soundfile
from PIL import Image

from rpy2psp.assets import audio, fonts, images, movies
from rpy2psp.assets.pack import PackOptions, build_pack, read_font_entry, read_pack, read_texture_entry, verify_pack
from rpy2psp.compiler.compile import compile_game
from rpy2psp.compiler.reader import load_game

SYSTEM_FONTS = [r"C:\Windows\Fonts\arial.ttf", r"C:\Windows\Fonts\segoeui.ttf",
                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf"]


def system_font():
    for p in SYSTEM_FONTS:
        if os.path.exists(p):
            return p
    raise unittest.SkipTest("no system TrueType font with Latin and Vietnamese glyphs found")


def gradient(w, h, alpha=False):
    y, x = np.mgrid[0:h, 0:w]
    a = np.stack([x * 255 // max(w - 1, 1), y * 255 // max(h - 1, 1), (x * 7 + y * 3) % 256,
                  (x * 255 // max(w - 1, 1)) if alpha else np.full_like(x, 255)], -1)
    return Image.fromarray(a.astype(np.uint8), "RGBA")


def dds(raw, w, h, fourcc):
    """Standard DXT decode through Pillow's DDS reader (independent of our decoder)."""
    hdr = struct.pack("<4sIIIIIII44xII4sIIIII4xIIII", b"DDS ", 124, 0x81007, h, w, len(raw), 0, 1,
                      32, 4, fourcc, 0, 0, 0, 0, 0, 0x1000, 0, 0, 0)
    return np.asarray(Image.open(io.BytesIO(hdr + raw)).convert("RGBA"))


class ImageTest(unittest.TestCase):
    def test_ge_decoder_matches_standard_dxt(self):
        for fmt, fourcc, alpha in ((images.GE_DXT1, b"DXT1", False), (images.GE_DXT5, b"DXT5", True)):
            a = np.asarray(gradient(64, 32, alpha))
            raw = etcpak.compress_bc1(a.tobytes(), 64, 32) if fmt == images.GE_DXT1 else \
                etcpak.compress_bc3(a.tobytes(), 64, 32)
            std = dds(raw, 64, 32, fourcc)
            ge = images.decode_blocks(images._dds_to_psp(raw, fmt), fmt, 64, 32)
            self.assertTrue(np.array_equal(ge[..., 3], std[..., 3]), fourcc)
            # The GE expands RGB565 without bit replication: at most 7 below the standard decode.
            diff = std[..., :3].astype(int) - ge[..., :3].astype(int)
            self.assertGreaterEqual(diff.min(), 0)
            self.assertLessEqual(diff.max(), 7)

    def test_psp_block_layout(self):
        raw = etcpak.compress_bc1(bytes([255, 0, 0, 255]) * 16, 4, 4)
        psp = images._dds_to_psp(raw, images.GE_DXT1)
        self.assertEqual(psp[:4], raw[4:8])
        self.assertEqual(psp[4:], raw[:4])
        raw5 = etcpak.compress_bc3(bytes(range(64)), 4, 4)
        psp5 = images._dds_to_psp(raw5, images.GE_DXT5)
        self.assertEqual(psp5[:4], raw5[12:16])
        self.assertEqual(psp5[4:8], raw5[8:12])
        self.assertEqual(psp5[8:14], raw5[2:8])
        self.assertEqual(psp5[14:16], raw5[0:2])

    def test_convert_scales_tiles_and_keeps_quality(self):
        src = gradient(1920, 720)
        tex, small = images.convert_image(src, images.screen_scale((1280, 720)))
        self.assertEqual((tex.width, tex.height), (720, 270))
        self.assertEqual(tex.fmt, images.GE_DXT1)
        self.assertEqual([(t["x"], t["tex_w"], t["rows"]) for t in tex.tiles], [(0, 512, 272), (512, 256, 272)])
        m = images.compare(small, images.decode_texture(tex))
        self.assertGreater(m["psnr"], 27)  # the blue channel is a high-frequency sawtooth
        self.assertEqual(m["alpha_max"], 0)
        tex5, small5 = images.convert_image(gradient(200, 100, alpha=True), 1.0)
        self.assertEqual(tex5.fmt, images.GE_DXT5)
        self.assertLess(images.compare(small5, images.decode_texture(tex5))["alpha_mean"], 2)

    def test_swizzle(self):
        data = bytes(range(256)) * 2  # 32 bytes wide, 16 rows
        sw = images.swizzle(data, 32, 16)
        self.assertEqual(sw[:16], data[:16])
        self.assertEqual(sw[16:32], data[32:48])  # second row of the first 16x8 block
        self.assertEqual(sw[128:144], data[16:32])  # first row of the second block
        self.assertEqual(images.unswizzle(sw, 32, 16), data)

    def test_recipes(self):
        root = tempfile.mkdtemp(prefix="rpy2psp-img-")
        self.addCleanup(shutil.rmtree, root, True)
        Image.new("RGBA", (10, 20), (255, 0, 0, 255)).save(os.path.join(root, "a.png"))
        game = load_game(root)
        comp = images.render_recipe(game, {"op": "composite", "size": [30, 30], "parts": [
            [0, 0, {"op": "solid", "color": [0, 0, 255, 255]}],
            [5, 5, {"op": "factor", "factor": [0.5, 0.5], "child": {"op": "file", "path": "a.png"}}]]})
        a = np.asarray(comp)
        self.assertEqual(tuple(a[0, 0]), (0, 0, 255, 255))
        self.assertEqual(tuple(a[7, 7]), (255, 0, 0, 255))
        crop = images.render_recipe(game, {"op": "crop", "rect": [0, 0, 4, 4], "child": {"op": "file", "path": "a.png"}})
        self.assertEqual(crop.size, (4, 4))
        grey = images.render_recipe(game, {"op": "grayscale", "child": {"op": "file", "path": "a.png"}})
        self.assertEqual(np.asarray(grey)[0, 0, 0], np.asarray(grey)[0, 0, 1])


class FontTest(unittest.TestCase):
    def test_atlas_round_trip_with_vietnamese_and_outline(self):
        with open(system_font(), "rb") as f:
            data = f.read()
        text = "Tiếng Việt: Chào buổi sáng, đường phố. ABC xyz 0123"
        cov = fonts.coverage_of(data)
        if any(ord(c) not in cov for c in text):
            self.skipTest("system font lacks Vietnamese glyphs")
        for outline in (0, 1):
            atlas = fonts.build_atlas(data, 14, text, outline)
            self.assertEqual(atlas.fmt, fonts.GE_T8 if outline else fonts.GE_T4)
            self.assertEqual(fonts.verify_atlas(data, atlas), 0)
            self.assertIn(ord("ệ"), atlas.glyphs)
            self.assertGreater(atlas.glyphs[ord("W" if "W" in text else "A")]["advance"], 0)


class AudioTest(unittest.TestCase):
    def tone(self, seconds, chans=2):
        t = np.arange(int(44100 * seconds)) / 44100
        x = [0.4 * np.sin(2 * np.pi * 440 * t), 0.3 * np.sin(2 * np.pi * 660 * t)][:chans]
        return np.stack(x, 1).astype(np.float32)

    def test_mp3_from_ogg_source(self):
        buf = io.BytesIO()
        soundfile.write(buf, self.tone(2.0), 44100, format="OGG", subtype="VORBIS")
        frames, rate = audio.decode_source(buf.getvalue(), "x.ogg")
        mp3, ref = audio.encode_mp3(frames, rate)
        self.assertEqual(audio.check_mp3_header(mp3), (1, 3, 128, 44100))
        m = audio.verify_mp3(mp3, ref)
        self.assertEqual(m["channels"], 2)
        self.assertLess(abs(m["duration_diff"]), 0.05)
        self.assertGreater(m["snr"], 15)

    def test_effects_pcm_and_adpcm(self):
        blob, ref, codec = audio.convert_effect(self.tone(0.5, 1), 44100)
        self.assertEqual(codec, audio.CODEC_PCM16)
        dec, rate = audio.decode_effect(blob)
        self.assertTrue(np.array_equal(dec, ref))
        blob, ref, codec = audio.convert_effect(self.tone(2.0), 22050)
        self.assertEqual(codec, audio.CODEC_IMA_ADPCM)
        dec, rate = audio.decode_effect(blob)
        self.assertEqual(rate, 22050)
        self.assertGreater(audio.snr_db(ref, dec), 25)
        self.assertLess(len(blob), ref.nbytes / 3.5)

    def test_rejects_garbage(self):
        with self.assertRaises(images.AssetError):
            audio.decode_source(b"not audio", "x.ogg")


class PackTest(unittest.TestCase):
    def test_compile_pack_verify(self):
        font = system_font()
        root = tempfile.mkdtemp(prefix="rpy2psp-pack-")
        self.addCleanup(shutil.rmtree, root, True)
        game = os.path.join(root, "game")
        os.makedirs(os.path.join(game, "images"))
        os.makedirs(os.path.join(game, "audio"))
        gradient(1280, 720).save(os.path.join(game, "images", "bg hall.png"))
        gradient(300, 600, alpha=True).save(os.path.join(game, "images", "eve happy.png"))
        t = np.arange(44100 * 2) / 44100
        soundfile.write(os.path.join(game, "audio", "theme.ogg"),
                        np.stack([0.3 * np.sin(2 * np.pi * 220 * t)] * 2, 1), 44100, format="OGG")
        soundfile.write(os.path.join(game, "audio", "click.wav"), 0.5 * np.sin(2 * np.pi * 900 * t[:4000]), 44100)
        shutil.copy(font, os.path.join(game, "body.ttf"))
        with open(os.path.join(game, "script.rpy"), "w", encoding="utf-8") as f:
            f.write('define gui.text_font = "body.ttf"\ndefine e = Character("Eve")\n'
                    'label start:\n    play music theme\n    scene bg hall\n    show eve happy\n'
                    '    play sound click\n    e "Xin chào!"\n    jump second\n'
                    'label second:\n    scene bg hall\n    "Again."\n    return\n')
        comp = compile_game(root)
        self.assertEqual(comp.errors, [])
        manifest = comp.manifest()
        manifest["source_size"] = [1280, 720]
        g = load_game(root)
        self.addCleanup(g.close)
        data, report = build_pack(g, manifest, PackOptions())
        entries, scenes = read_pack(data)
        self.assertEqual(set(scenes), {"start", "second"})
        self.assertEqual(len(scenes["start"]), 4)
        tex = read_texture_entry(entries[scenes["second"][0]][1])
        self.assertEqual((tex.width, tex.height, tex.source_size), (480, 270, (1280, 720)))
        self.assertEqual(entries["font/text"][1], entries["font/name"][1])
        atlas = read_font_entry(entries["font/text"][1])
        self.assertIn(ord("à"), atlas.glyphs)
        summary, failures = verify_pack(data, g, manifest, PackOptions())
        self.assertEqual(failures, [])
        self.assertEqual(len(summary["image"]), 2)
        self.assertEqual(summary["sfx"][0]["codec"], audio.CODEC_PCM16)

    def test_missing_font_is_a_clear_error(self):
        root = tempfile.mkdtemp(prefix="rpy2psp-pack-")
        self.addCleanup(shutil.rmtree, root, True)
        os.makedirs(os.path.join(root, "game"))
        g = load_game(root)
        with self.assertRaises(images.AssetError) as cm:
            fonts.find_font(g, "Nope.ttf")
        self.assertIn("Nope.ttf", str(cm.exception))


class MovieTest(unittest.TestCase):
    def test_h264_baseline_480x272(self):
        if not (shutil.which("ffmpeg") and shutil.which("ffprobe")):
            self.skipTest("ffmpeg/ffprobe not on PATH")
        tmp = tempfile.mkdtemp(prefix="rpy2psp-mov-")
        self.addCleanup(shutil.rmtree, tmp, True)
        src = os.path.join(tmp, "clip.mp4")
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc=size=1280x720:rate=30:duration=1",
                        "-f", "lavfi", "-i", "sine=frequency=440:duration=1", "-shortest", "-pix_fmt", "yuv420p",
                        src], check=True)
        with open(src, "rb") as f:
            entry = movies.convert_movie(f.read(), "clip.mp4")
        info = movies.probe_movie(entry)
        self.assertEqual((info["codec"], info["width"], info["height"]), ("h264", 480, 272))
        self.assertIn("Baseline", info["profile"])
        mp3_len = struct.unpack_from("<I", entry, 8)[0]
        self.assertEqual(audio.check_mp3_header(entry[-mp3_len:])[:2], (1, 3))


if __name__ == "__main__":
    unittest.main()
