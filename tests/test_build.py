"""EBOOT/PARAM.SFO writing and the `rpy2psp build` command, on generated inputs."""

import contextlib
import io
import os
import shutil
import struct
import tempfile
import unittest

from PIL import Image

from rpy2psp import eboot
from rpy2psp.cli import main as cli_main

SYSTEM_FONTS = [r"C:\Windows\Fonts\arial.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                "/System/Library/Fonts/Supplemental/Arial.ttf"]
ENGINE_SFO = {"MEMSIZE": 2, "APP_VER": "01.00", "BOOTABLE": 1, "CATEGORY": "MG", "DISC_ID": "UCJS10041",
              "DISC_VERSION": "1.00", "PARENTAL_LEVEL": 1, "PSP_SYSTEM_VER": "1.00", "REGION": 0x8000,
              "TITLE": "vnpsp"}
ELF = b"\x7fELF" + bytes(range(256)) * 4


def fake_engine():
    return eboot.write_pbp({"PARAM.SFO": eboot.write_sfo(ENGINE_SFO), "DATA.PSP": ELF})


def png(size, color, mode="RGBA"):
    buf = io.BytesIO()
    Image.new(mode, size, color).save(buf, "PNG")
    return buf.getvalue()


class SfoTest(unittest.TestCase):
    def test_round_trip_sorted_keys(self):
        data = eboot.write_sfo(ENGINE_SFO)
        self.assertEqual(eboot.read_sfo(data), ENGINE_SFO)
        self.assertEqual(list(eboot.read_sfo(data)), sorted(ENGINE_SFO))
        magic, version, key_start, data_start, count = struct.unpack_from("<4sIIII", data)
        self.assertEqual((magic, version, count), (b"\0PSF", 0x101, len(ENGINE_SFO)))
        self.assertEqual(data_start % 4, 0)

    def test_title_field_reserves_128_bytes(self):
        data = eboot.write_sfo({"TITLE": "ザ・クエスチョン"})
        _, fmt, length, max_len, _ = struct.unpack_from("<HHIII", data, 20)
        self.assertEqual((fmt, length, max_len), (eboot.FMT_UTF8, len("ザ・クエスチョン".encode()) + 1, 128))
        self.assertEqual(eboot.read_sfo(data)["TITLE"], "ザ・クエスチョン")

    def test_long_titles_are_cut_on_a_character_boundary(self):
        title = eboot.clip_title("あ" * 60)
        self.assertEqual(title, "あ" * 42)
        self.assertLessEqual(len(title.encode()), 127)
        eboot.write_sfo({"TITLE": title})
        with self.assertRaises(eboot.EbootError):
            eboot.write_sfo({"TITLE": "x" * 128})

    def test_bad_input_is_an_error(self):
        for bad in (b"", b"\0PSF", b"PSF\0" + bytes(16), b"\0PSF" + struct.pack("<IIII", 0x101, 99, 99, 50)):
            with self.subTest(bad=bad), self.assertRaises(eboot.EbootError):
                eboot.read_sfo(bad)


class PbpTest(unittest.TestCase):
    def test_round_trip(self):
        sections = {"PARAM.SFO": b"sfo", "ICON0.PNG": b"icon", "DATA.PSP": ELF}
        data = eboot.write_pbp(sections)
        self.assertEqual(data[:8], b"\0PBP\0\0\1\0")
        back = eboot.read_pbp(data)
        self.assertEqual({k: v for k, v in back.items() if v}, sections)

    def test_bad_pbp_is_an_error(self):
        good = eboot.write_pbp({"PARAM.SFO": b"sfo", "DATA.PSP": ELF})
        bad_offset = good[:8] + struct.pack("<I", len(good) + 10) + good[12:]
        for bad in (b"", b"\0PBX" + good[4:], bad_offset):
            with self.subTest(n=len(bad)), self.assertRaises(eboot.EbootError):
                eboot.read_pbp(bad)
        with self.assertRaises(eboot.EbootError):
            eboot.write_pbp({"EXTRA.BIN": b""})

    def test_make_eboot_keeps_the_engine_and_its_settings(self):
        out = eboot.make_eboot(fake_engine(), "  My Novel ", png((144, 80), "red"), png((480, 272), "blue", "RGB"))
        s = eboot.read_pbp(out)
        self.assertEqual(s["DATA.PSP"], ELF)
        sfo = eboot.read_sfo(s["PARAM.SFO"])
        self.assertEqual(sfo, dict(ENGINE_SFO, TITLE="My Novel"))
        self.assertTrue(s["ICON0.PNG"].startswith(b"\x89PNG"))
        self.assertTrue(s["PIC1.PNG"].startswith(b"\x89PNG"))
        with self.assertRaises(eboot.EbootError):
            eboot.make_eboot(eboot.write_pbp({"PARAM.SFO": eboot.write_sfo(ENGINE_SFO)}), "x")


class XmbImageTest(unittest.TestCase):
    def test_square_icon_is_centered_with_transparent_sides(self):
        with Image.open(io.BytesIO(eboot.icon0_from(png((256, 256), (255, 0, 0, 255)), "icon"))) as im:
            self.assertEqual(im.size, (144, 80))
            im = im.convert("RGBA")
            self.assertEqual(im.getpixel((0, 40))[3], 0)
            self.assertEqual(im.getpixel((72, 40)), (255, 0, 0, 255))

    def test_backgrounds_fill_the_frame(self):
        bg = png((1280, 720), (0, 128, 255, 255))
        with Image.open(io.BytesIO(eboot.icon0_from(bg, "background"))) as im:
            self.assertEqual(im.size, (144, 80))
            self.assertEqual(im.convert("RGBA").getpixel((0, 0)), (0, 128, 255, 255))
        with Image.open(io.BytesIO(eboot.pic1_from(bg))) as im:
            self.assertEqual((im.size, im.mode), ((480, 272), "RGB"))

    def test_ico_files_are_read(self):
        buf = io.BytesIO()
        Image.new("RGBA", (64, 64), (0, 255, 0, 255)).save(buf, "ICO", sizes=[(16, 16), (64, 64)])
        with Image.open(io.BytesIO(eboot.icon0_from(buf.getvalue(), "icon"))) as im:
            self.assertEqual(im.convert("RGBA").getpixel((72, 40)), (0, 255, 0, 255))

    def test_undecodable_image_is_an_error(self):
        with self.assertRaises(eboot.EbootError):
            eboot.pic1_from(b"not an image")


SCRIPT = '''
define config.name = "Tiny Tale"
define config.window_icon = "gui/window_icon.png"
define gui.main_menu_background = "gui/main_menu.png"
define gui.text_font = "body.ttf"
define gui.name_text_font = "body.ttf"
define gui.interface_text_font = "body.ttf"

label start:
    "Hello."
    menu:
        "Left":
            "Gone left."
        "Right":
            "Gone right."
    return
'''


class BuildCommandTest(unittest.TestCase):
    def setUp(self):
        font = next((p for p in SYSTEM_FONTS if os.path.exists(p)), None)
        if not font:
            self.skipTest("no system TrueType font")
        self.tmp = tempfile.mkdtemp(prefix="rpy2psp-build-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        game = os.path.join(self.tmp, "tiny", "game")
        os.makedirs(os.path.join(game, "gui"))
        with open(os.path.join(game, "script.rpy"), "w", encoding="utf-8") as f:
            f.write(SCRIPT)
        shutil.copy(font, os.path.join(game, "body.ttf"))
        with open(os.path.join(game, "gui", "window_icon.png"), "wb") as f:
            f.write(png((64, 64), (255, 255, 0, 255)))
        with open(os.path.join(game, "gui", "main_menu.png"), "wb") as f:
            f.write(png((1280, 720), (10, 20, 30, 255)))
        self.game = os.path.dirname(game)
        self.engine = os.path.join(self.tmp, "engine.pbp")
        with open(self.engine, "wb") as f:
            f.write(fake_engine())

    def run_cli(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = cli_main(list(args))
        return rc, out.getvalue(), err.getvalue()

    def test_build_writes_a_psp_game_folder(self):
        out = os.path.join(self.tmp, "TinyTale")
        rc, text, err = self.run_cli("build", self.game, "-o", out, "--engine", self.engine)
        self.assertEqual(rc, 0, err)
        for name in ("EBOOT.PBP", "script.vnb", "data.pak"):
            self.assertTrue(os.path.getsize(os.path.join(out, name)) > 0, name)
        with open(os.path.join(out, "EBOOT.PBP"), "rb") as f:
            s = eboot.read_pbp(f.read())
        self.assertEqual(s["DATA.PSP"], ELF)
        self.assertEqual(eboot.read_sfo(s["PARAM.SFO"])["TITLE"], "Tiny Tale")
        with Image.open(io.BytesIO(s["ICON0.PNG"])) as im:
            self.assertEqual(im.convert("RGBA").getpixel((72, 40)), (255, 255, 0, 255))
        with Image.open(io.BytesIO(s["PIC1.PNG"])) as im:
            self.assertEqual(im.getpixel((0, 0)), (10, 20, 30))
        self.assertIn("ms0:/PSP/SAVEDATA/TinyTale/", text)

    def test_options_override_title_and_images(self):
        icon = os.path.join(self.tmp, "icon.png")
        with open(icon, "wb") as f:
            f.write(png((144, 80), (0, 0, 255, 255)))
        out = os.path.join(self.tmp, "o")
        rc, _, err = self.run_cli("build", self.game, "-o", out, "--engine", self.engine, "--title", "Other",
                                  "--icon", icon)
        self.assertEqual(rc, 0, err)
        with open(os.path.join(out, "EBOOT.PBP"), "rb") as f:
            s = eboot.read_pbp(f.read())
        self.assertEqual(eboot.read_sfo(s["PARAM.SFO"])["TITLE"], "Other")
        with Image.open(io.BytesIO(s["ICON0.PNG"])) as im:
            self.assertEqual(im.convert("RGBA").getpixel((0, 0)), (0, 0, 255, 255))

    def test_missing_engine_is_reported_before_any_work(self):
        out = os.path.join(self.tmp, "none")
        rc, _, err = self.run_cli("build", self.game, "-o", out, "--engine", os.path.join(self.tmp, "nope.pbp"))
        self.assertEqual(rc, 1)
        self.assertIn("not found", err)
        self.assertFalse(os.path.exists(out))

    def test_unsupported_script_writes_nothing(self):
        with open(os.path.join(self.game, "game", "extra.rpy"), "w", encoding="utf-8") as f:
            f.write("label extra:\n    $ renpy.call_screen('custom_thing')\n    return\n")
        out = os.path.join(self.tmp, "bad")
        rc, _, err = self.run_cli("build", self.game, "-o", out, "--engine", self.engine)
        self.assertEqual(rc, 1, err)
        self.assertIn("extra.rpy", err)
        self.assertFalse(os.path.exists(os.path.join(out, "EBOOT.PBP")))


if __name__ == "__main__":
    unittest.main()
