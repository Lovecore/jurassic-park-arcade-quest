"""Repo and game-folder locations for the analysis tools.

The game folder is your own TP dump (never committed): $JP2015_GAME, or a
`jp_game` symlink at the repo root.
"""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
GAME_DIR = Path(os.environ.get("JP2015_GAME", ROOT / "jp_game"))
SYMBOLS = GAME_DIR / "symbols_original.txt"


def pristine_game():
    """The unmodified TP binary (game.q0bak if kept, else the dump's game)."""
    for name in ("game.q0bak", "game.orig", "game"):
        p = GAME_DIR / name
        if p.exists():
            return str(p)
    raise FileNotFoundError(f"no game binary in {GAME_DIR} (set JP2015_GAME)")
