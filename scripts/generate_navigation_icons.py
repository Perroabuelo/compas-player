#!/usr/bin/env python3
"""Render navigation Phosphor icons for common and board-specific themes.

The tracked SVGs use @phosphor-icons/core@2.1.1 regular paths (MIT) with
Compas gradients. Requires ffmpeg with librsvg and Pillow; render at 176px
before Lanczos downsampling to 44px.
"""
from pathlib import Path
import subprocess
import tempfile

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "scripts/assets/navigation_icons"
OUTPUT_DIRS = (
    ROOT / "assets/theme2/navigation",
    ROOT / "assets/r3ii_2025/theme2/navigation",
    ROOT / "assets/r3proii/theme2/navigation",
)
ICONS = ("search", "more")

with tempfile.TemporaryDirectory(prefix="navigation-icons-") as temp_dir:
    temp_dir = Path(temp_dir)
    for name in ICONS:
        high_res = temp_dir / f"{name}-176.png"
        subprocess.run(
            [
                "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                "-i", str(SOURCE_DIR / f"{name}.svg"), "-frames:v", "1",
                "-vf", "scale=176:176", str(high_res),
            ],
            check=True,
        )
        with Image.open(high_res) as rendered:
            icon = rendered.convert("RGBA").resize((44, 44), Image.Resampling.LANCZOS)
        for output_dir in OUTPUT_DIRS:
            output_dir.mkdir(parents=True, exist_ok=True)
            output = output_dir / f"{name}.png"
            icon.save(output, format="PNG", optimize=True)
            print(output)
