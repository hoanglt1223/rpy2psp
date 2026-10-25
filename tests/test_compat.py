"""compat/*.toml follow compat/schema.md."""

import datetime
import glob
import os
import re
import tomllib
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
STAGES = ["fails", "boots", "menus", "in-game", "playable", "perfect"]
FIELDS = {"title": str, "kind": str, "source": str, "commit": str, "game": str, "license": str, "languages": list,
          "stage": str, "evidence": str, "blockers": list, "updated": datetime.date}


class CompatTest(unittest.TestCase):
    def test_entries(self):
        paths = sorted(glob.glob(os.path.join(ROOT, "compat", "*.toml")))
        self.assertTrue(paths)
        for path in paths:
            with self.subTest(file=os.path.basename(path)):
                with open(path, "rb") as f:
                    data = tomllib.load(f)
                self.assertEqual(set(data), set(FIELDS))
                for key, kind in FIELDS.items():
                    self.assertIsInstance(data[key], kind, key)
                self.assertIn(data["kind"], ("homebrew", "commercial"))
                self.assertIn(data["stage"], STAGES)
                self.assertRegex(data["commit"], r"^[0-9a-f]{40}$")
                self.assertTrue(data["source"].startswith("https://"))
                self.assertTrue(data["evidence"].strip())
                if data["stage"] != "perfect":
                    self.assertTrue(data["blockers"], "a stage below perfect names what blocks the next one")
                self.assertTrue(re.fullmatch(r"[a-z0-9-]+\.toml", os.path.basename(path)))


if __name__ == "__main__":
    unittest.main()
