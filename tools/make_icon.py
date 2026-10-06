"""Generate the project icon.

Checked in as a script rather than only as a binary so the mark can be changed
without a drawing tool. Run it after editing the geometry:

    python tools/make_icon.py

Writes native/assets/ghost.ico with every size Windows asks for. The icon is
used twice: as the executable's own icon, and as the icon the shim applies to the
browser window so the taskbar shows the project rather than the engine.
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "native" / "assets" / "ghost.ico"

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
SUPERSAMPLE = 8

BACKDROP_TOP = (18, 30, 54)      # deep navy
BACKDROP_BOTTOM = (12, 20, 38)
GHOST = (236, 248, 255)          # near-white with a cold cast
GHOST_SHADE = (176, 208, 232)
EYE = (16, 26, 46)


def rounded_square(size: int, radius_ratio: float = 0.22) -> Image.Image:
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    radius = int(size * radius_ratio)
    for y in range(size):
        t = y / max(1, size - 1)
        colour = tuple(
            int(BACKDROP_TOP[i] + (BACKDROP_BOTTOM[i] - BACKDROP_TOP[i]) * t) for i in range(3)
        )
        draw.line([(0, y), (size, y)], fill=colour + (255,))
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, size - 1, size - 1], radius=radius, fill=255)
    layer.putalpha(mask)
    return layer


def ghost_mask(size: int) -> Image.Image:
    """The silhouette: a domed head, straight sides, a scalloped hem."""
    mask = Image.new("L", (size, size), 0)
    draw = ImageDraw.Draw(mask)

    margin_x = size * 0.20
    top = size * 0.16
    hem = size * 0.78
    left = margin_x
    right = size - margin_x
    width = right - left

    # Domed head: a half-disc whose diameter is the body width.
    draw.pieslice([left, top, right, top + width], start=180, end=360, fill=255)
    # Body.
    draw.rectangle([left, top + width / 2, right, hem], fill=255)
    # Hem: overlapping discs centred on the hem line scallop the bottom edge.
    scallops = 3
    step = width / scallops
    for i in range(scallops):
        cx = left + step * (i + 0.5)
        r = step / 2
        draw.ellipse([cx - r, hem - r, cx + r, hem + r], fill=255)
    return mask


def draw_icon(size: int) -> Image.Image:
    big = size * SUPERSAMPLE
    icon = rounded_square(big)

    body = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    body.paste(GHOST + (255,), (0, 0), ghost_mask(big))

    # Eyes, punched out of the silhouette.
    eyes = ImageDraw.Draw(body)
    eye_w = big * 0.115
    eye_h = big * 0.165
    eye_y = big * 0.42
    for cx in (big * 0.395, big * 0.605):
        eyes.ellipse([cx - eye_w / 2, eye_y - eye_h / 2, cx + eye_w / 2, eye_y + eye_h / 2],
                     fill=EYE + (255,))

    icon = Image.alpha_composite(icon, body)
    return icon.resize((size, size), Image.LANCZOS)


def main() -> int:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    frames = [draw_icon(size) for size in SIZES]
    # Pillow writes every supplied size into the .ico, which is what makes the
    # taskbar crisp at 16 px and the alt-tab preview crisp at 256.
    frames[-1].save(OUT, format="ICO", sizes=[(s, s) for s in SIZES],
                    append_images=frames[:-1])
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes, {len(SIZES)} sizes)")
    preview = ROOT / ".tmp" / "icon-preview.png"
    preview.parent.mkdir(parents=True, exist_ok=True)
    sheet = Image.new("RGBA", (sum(SIZES) + 8 * (len(SIZES) + 1), 272), (255, 255, 255, 255))
    x = 8
    for size, frame in zip(SIZES, frames):
        sheet.alpha_composite(frame, (x, 8))
        x += size + 8
    sheet.save(preview)
    print(f"preview {preview}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
