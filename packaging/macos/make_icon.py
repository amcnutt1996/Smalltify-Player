#!/usr/bin/env python3
"""Regenerate the logo's derivatives from packaging/logo.png (a 1024 px
tile with transparent corners):

  - packaging/macos/AppIcon.icns: the tile scaled to Apple's 824 px plate
    on the 1024 grid (100 px transparent margins), at every icon size;
  - src/gui/logo.png: the 256 px copy compiled into the app (the mark at
    the head of the sign-in cards and on the Settings sheet).

macOS only (`sips`, `iconutil`). Both outputs are committed, so builds
never need this script.

    python3 packaging/macos/make_icon.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LOGO = os.path.join(ROOT, "packaging", "logo.png")
APP_LOGO = os.path.join(ROOT, "src", "gui", "logo.png")
PLATE = 824 / 1024  # share of the icon canvas the tile covers


def sips(*args):
    subprocess.run(["sips", *args], check=True, stdout=subprocess.DEVNULL)


def main():
    for tool in ("sips", "iconutil"):
        if not shutil.which(tool):
            sys.exit(tool + " not found (macOS only)")
    work = tempfile.mkdtemp(dir=HERE)  # stays inside the repo, removed below
    try:
        iconset = os.path.join(work, "AppIcon.iconset")
        os.mkdir(iconset)
        for base in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                px = base * scale
                name = "icon_%dx%d%s.png" % (base, base, "@2x" if scale == 2 else "")
                out = os.path.join(iconset, name)
                plate = max(round(px * PLATE), 1)
                sips("-z", str(plate), str(plate), LOGO, "--out", out)
                sips("--padToHeightWidth", str(px), str(px), out)  # transparent
        icns = os.path.join(HERE, "AppIcon.icns")
        subprocess.run(["iconutil", "-c", "icns", "-o", icns, iconset], check=True)
        print("wrote", icns)
    finally:
        shutil.rmtree(work)
    sips("-z", "256", "256", LOGO, "--out", APP_LOGO)
    print("wrote", APP_LOGO)


if __name__ == "__main__":
    main()
