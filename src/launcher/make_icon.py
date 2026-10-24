"""Draws the launcher's icon, rogue-galaxy.ico: a ringed planet.

Drawn from nothing (no game art), at 1024 pixels and scaled down to each size
Windows asks for. Needs Pillow:

    python src/launcher/make_icon.py
"""

import math
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

SIZE = 1024
CENTRE = SIZE / 2
PLANET = 300
TILT = -22.0
OUT = Path(__file__).with_name("rogue-galaxy.ico")


def planet() -> Image.Image:
    """The planet: lit from the upper left, banded, darker at its rim."""
    light = (-0.55, -0.6, 0.58)
    norm = math.sqrt(sum(c * c for c in light))
    light = tuple(c / norm for c in light)
    angle = math.radians(TILT)
    deep, bright, band = (38, 22, 96), (96, 196, 255), (170, 110, 255)
    pixels = []
    for row in range(SIZE):
        dy = (row + 0.5 - CENTRE) / PLANET
        for column in range(SIZE):
            dx = (column + 0.5 - CENTRE) / PLANET
            r2 = dx * dx + dy * dy
            if r2 > 1.0:
                pixels.append((0, 0, 0, 0))
                continue
            dz = math.sqrt(1.0 - r2)
            shade = min(max(dx * light[0] + dy * light[1] + dz * light[2], 0.0), 1.0)
            # Bands across the tilted axis, as a gas giant's.
            across = -dx * math.sin(angle) + dy * math.cos(angle)
            bands = 0.5 + 0.5 * math.sin(across * 9.0 + math.sin(across * 3.0) * 1.5)
            lit = shade ** 0.9
            dark = 0.35 + 0.65 * math.sqrt(shade)
            colour = []
            for d, b, c in zip(deep, bright, band):
                value = d + (b - d) * lit
                value += (c - value) * bands * 0.28 * shade
                colour.append(int(min(max(value * dark, 0), 255)))
            pixels.append((*colour, 255))
    picture = Image.new("RGBA", (SIZE, SIZE))
    picture.putdata(pixels)
    return picture


def ring(front: bool) -> Image.Image:
    """The ring's half in front of the planet (or behind it), tilted."""
    layer = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    for outer, inner, colour in (
        ((500, 150), (440, 124), (255, 214, 120, 255)),
        ((430, 120), (370, 98), (240, 160, 80, 235)),
    ):
        draw.ellipse(
            (CENTRE - outer[0], CENTRE - outer[1], CENTRE + outer[0], CENTRE + outer[1]),
            fill=colour,
        )
        draw.ellipse(
            (CENTRE - inner[0], CENTRE - inner[1], CENTRE + inner[0], CENTRE + inner[1]),
            fill=(0, 0, 0, 0),
        )
    # The near half is the lower one, before the tilt.
    half = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(half).rectangle(
        (0, CENTRE if front else 0, SIZE, SIZE if front else CENTRE), fill=255
    )
    layer.putalpha(ImageChops.multiply(layer.getchannel("A"), half))
    return layer.rotate(-TILT, resample=Image.BICUBIC, center=(CENTRE, CENTRE))


def main() -> None:
    picture = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    # A soft glow behind it all, so it reads on dark and light desktops.
    glow = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse(
        (CENTRE - PLANET - 40, CENTRE - PLANET - 40, CENTRE + PLANET + 40, CENTRE + PLANET + 40),
        fill=(120, 170, 255, 110),
    )
    picture.alpha_composite(glow.filter(ImageFilter.GaussianBlur(40)))
    picture.alpha_composite(ring(front=False))
    picture.alpha_composite(planet())
    picture.alpha_composite(ring(front=True))
    sizes = [(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (24, 24), (16, 16)]
    picture.save(OUT, sizes=sizes)
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
