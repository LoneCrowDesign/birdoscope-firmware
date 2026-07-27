# Copyright (C) 2026 Lone Crow Design, LLC
# Licensed under the MIT License. See LICENSE.
#
# Converts the raw display buffers render.cpp dumps into PNGs.
#
# u8g2's full-buffer layout is page-major: byte index (page * 128 + x) holds a
# vertical run of 8 pixels, bit 0 at the top of the page. Pixels are drawn white
# on black to match a lit OLED, and upscaled with nearest-neighbour so the pixel
# grid stays crisp rather than blurring into grey.

import sys
from pathlib import Path

from PIL import Image

WIDTH = 128
HEIGHT = 64

# Output width in pixels; height follows the panel's 2:1 ratio. Keep this a
# multiple of 128 so every source pixel maps to the same number of output
# columns: 384 is a clean 3x, 512 a 4x. An off-multiple width such as 400 still
# renders, but nearest-neighbour then gives some pixels 3 columns and others 4,
# which shows up as uneven stroke weights in the text.
OUT_WIDTH = 384


def buffer_to_image(raw: bytes) -> Image.Image:
    img = Image.new("RGB", (WIDTH, HEIGHT), (0, 0, 0))
    px = img.load()
    for page in range(HEIGHT // 8):
        for x in range(WIDTH):
            byte = raw[page * WIDTH + x]
            for bit in range(8):
                if byte & (1 << bit):
                    px[x, page * 8 + bit] = (255, 255, 255)
    return img.resize((OUT_WIDTH, OUT_WIDTH * HEIGHT // WIDTH), Image.NEAREST)


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: to_png.py <dir-of-bin-files>", file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    bins = sorted(out.glob("*.bin"))
    if not bins:
        print(f"no .bin files in {out}", file=sys.stderr)
        return 1
    for b in bins:
        raw = b.read_bytes()
        expected = WIDTH * HEIGHT // 8
        if len(raw) != expected:
            print(f"{b.name}: expected {expected} bytes, got {len(raw)}", file=sys.stderr)
            return 1
        buffer_to_image(raw).save(b.with_suffix(".png"))
        b.unlink()
        print(b.with_suffix(".png").name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
