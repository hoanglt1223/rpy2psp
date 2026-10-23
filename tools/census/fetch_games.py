"""Fetch the census corpus listed in games.json into a scratch folder.

    python -I fetch_games.py <dest-dir>        (default: %TEMP%/rpy2psp-census/games)

Each game is a blobless, shallow git clone at the pinned commit with only *.rpy
files checked out, plus <name>.files.txt (the full file listing, used for the
asset-format census). No game assets are downloaded and nothing is executed:
git hooks are disabled and Git LFS smudging is skipped. Keep the destination
outside the repository.
"""

import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def git(*args, cwd=None):
    env = dict(os.environ, GIT_LFS_SKIP_SMUDGE="1", GIT_TERMINAL_PROMPT="0")
    return subprocess.run(["git", "-c", "core.hooksPath=" + os.devnull, *args], cwd=cwd, env=env,
                          check=True, capture_output=True, text=True).stdout


def fetch(game, dest):
    path = os.path.join(dest, game["name"])
    if not os.path.isdir(path):
        os.makedirs(path)
        git("init", "-q", cwd=path)
        git("remote", "add", "origin", game["repo"], cwd=path)
        git("fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", game["commit"], cwd=path)
        git("sparse-checkout", "set", "--no-cone", "*.rpy", cwd=path)
        git("checkout", "-q", "FETCH_HEAD", cwd=path)
    listing = git("ls-tree", "-r", "--name-only", "HEAD", cwd=path)
    with open(os.path.join(dest, game["name"] + ".files.txt"), "w", encoding="utf-8") as f:
        f.write(listing)


def main():
    dest = sys.argv[1] if len(sys.argv) > 1 else os.path.join(tempfile.gettempdir(), "rpy2psp-census", "games")
    os.makedirs(dest, exist_ok=True)
    with open(os.path.join(HERE, "games.json"), encoding="utf-8") as f:
        games = json.load(f)["games"]
    for g in games:
        print("fetching", g["name"], flush=True)
        fetch(g, dest)
    print("done:", dest)


if __name__ == "__main__":
    main()
