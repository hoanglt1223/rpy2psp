"""`rpy2psp build`: compile, pack, and wrap the engine into a folder ready for
ms0:/PSP/GAME/ (EBOOT.PBP with the game's title and images, script.vnb, data.pak)."""

import os
import sys

from . import eboot

ENGINE_ENV = "RPY2PSP_ENGINE"
ICON_FALLBACKS = ("gui/window_icon.png", "gui/window_icon.webp", "gui/icon.png", "icon.png")
MENU_FALLBACKS = ("gui/main_menu.png", "gui/main_menu.jpg", "gui/main_menu.webp")


def engine_candidates():
    """Where the engine EBOOT is looked for, in order."""
    out = []
    env = os.environ.get(ENGINE_ENV)
    if env:
        out.append(env)
    if getattr(sys, "frozen", False):
        out.append(os.path.join(os.path.dirname(sys.executable), "vnpsp", "EBOOT.PBP"))
    here = os.path.dirname(os.path.abspath(__file__))
    out.append(os.path.join(here, "vnpsp", "EBOOT.PBP"))
    out.append(os.path.join(os.path.dirname(here), "out", "psp", "vnpsp", "EBOOT.PBP"))
    return out


def find_engine(explicit=None):
    if explicit:
        if not os.path.isfile(explicit):
            raise eboot.EbootError(f"engine EBOOT {explicit!r} not found")
        return explicit
    for path in engine_candidates():
        if os.path.isfile(path):
            return path
    raise eboot.EbootError("the vnpsp engine EBOOT was not found; pass --engine <EBOOT.PBP> or set "
                           f"{ENGINE_ENV} (release zips have it in vnpsp/, or build it with scripts/psp-build.sh)")


def _game_image(game, configured, fallbacks):
    """The first readable image among the configured path and the usual gui/ files.
    Configured values that are colours or displayables are ignored."""
    names = [configured] if isinstance(configured, str) and not configured.startswith("#") else []
    for name in names + list(fallbacks):
        if game.exists(name):
            return name, game.read(name)
    return None, None


def xmb_images(game, static, icon_path=None, background_path=None):
    """Returns (icon0 PNG or None, pic1 PNG or None, notes)."""
    notes = []

    def from_file(path):
        with open(path, "rb") as f:
            return f.read()

    if icon_path:
        icon_name, icon = icon_path, from_file(icon_path)
    else:
        icon_name, icon = _game_image(game, static.get("config.window_icon"), ICON_FALLBACKS)
    if background_path:
        menu_name, menu = background_path, from_file(background_path)
    else:
        menu_name, menu = _game_image(game, static.get("gui.main_menu_background"), MENU_FALLBACKS)
    icon0 = pic1 = None
    try:
        if icon is not None:
            icon0 = eboot.icon0_from(icon, "icon")
        elif menu is not None:
            icon0 = eboot.icon0_from(menu, "background")
            icon_name = menu_name
    except eboot.EbootError as e:
        notes.append(f"icon {icon_name}: {e}; no ICON0")
        icon0, icon_name = None, None
    try:
        if menu is not None:
            pic1 = eboot.pic1_from(menu)
    except eboot.EbootError as e:
        notes.append(f"menu image {menu_name}: {e}; no PIC1")
        pic1, menu_name = None, None
    notes.append(f"ICON0: {icon_name or 'none (the XMB shows its default icon)'}; "
                 f"PIC1: {menu_name if pic1 else 'none'}")
    return icon0, pic1, notes


def game_title(config):
    for key in ("name", "window_title"):
        if isinstance(config.get(key), str) and config[key].strip():
            return config[key]
    return None
