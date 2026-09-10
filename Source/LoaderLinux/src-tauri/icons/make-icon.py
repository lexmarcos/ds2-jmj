#!/usr/bin/env python3
"""Draws the ds2os app icon.

The mark is the bonfire: a coiled sword driven point-down, read as a dark
silhouette against the fire. There is no drawn flame - the glow alone carries
it, which is what keeps the sword legible once the icon is 48px in a dock.

It follows the interface's rules: the ground is night rather than black, there
is exactly one warm light, and pure white never appears.

Regenerate with `python3 make-icon.py`, then `npx tauri icon icons/icon.png`
for the platform sizes.
"""

from PIL import Image, ImageDraw, ImageFilter

SIZE = 1024
VOID = (15, 20, 32, 255)
EMBER = (200, 145, 63)
EMBER_BRIGHT = (240, 199, 122)


def draw_glow(image):
    """Two nested pools of light, taller than wide so it reads as fire."""
    layer = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    draw.ellipse((SIZE * 0.22, SIZE * 0.36, SIZE * 0.78, SIZE * 0.95), fill=(*EMBER, 215))
    draw.ellipse((SIZE * 0.34, SIZE * 0.50, SIZE * 0.66, SIZE * 0.87), fill=(*EMBER_BRIGHT, 240))
    image.alpha_composite(layer.filter(ImageFilter.GaussianBlur(SIZE * 0.072)))


def draw_sword(draw):
    """Blade point-down, with crossguard, grip and pommel."""
    cx = SIZE * 0.5
    top, bottom = SIZE * 0.18, SIZE * 0.90
    guard_y = top + (bottom - top) * 0.34
    half = SIZE * 0.036

    draw.polygon(
        [
            (cx - half, guard_y),
            (cx + half, guard_y),
            (cx + half, bottom - SIZE * 0.11),
            (cx, bottom),
            (cx - half, bottom - SIZE * 0.11),
        ],
        fill=VOID,
    )
    draw.rounded_rectangle(
        (cx - SIZE * 0.140, guard_y - SIZE * 0.027, cx + SIZE * 0.140, guard_y + SIZE * 0.014),
        radius=SIZE * 0.011,
        fill=VOID,
    )
    draw.rectangle((cx - SIZE * 0.020, top + SIZE * 0.054, cx + SIZE * 0.020, guard_y), fill=VOID)
    draw.ellipse((cx - SIZE * 0.041, top, cx + SIZE * 0.041, top + SIZE * 0.075), fill=VOID)


def main():
    image = Image.new("RGBA", (SIZE, SIZE), VOID)
    draw_glow(image)
    draw_sword(ImageDraw.Draw(image))
    image.save("icon.png")
    print("wrote icon.png")


if __name__ == "__main__":
    main()
