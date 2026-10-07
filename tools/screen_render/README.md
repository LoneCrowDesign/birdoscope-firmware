# screen_render

Renders the OLED screen carousel to PNGs on the host, with no device attached.

```
./tools/screen_render/render.sh <output-dir>
```

`render.sh` requires the output directory. The run writes `.bin` intermediates
and an orientation probe, `00_probe.png`, into it. Point it at a scratch
directory, not at `assets/images/carousel_demo`, and copy the PNGs across once
they look right.

The script needs `gcc`, `g++`, `python3` with Pillow, and a populated
`.pio/libdeps`. Run `pio run -e <env>` once with an OLED board env to fetch
u8g2.

## How It Works

`render.cpp` includes `src/screens.inc`, the firmware's own drawing code, and
`render.sh` compiles it against u8g2's plain-C sources built natively.
`render.cpp` supplies stubs for everything the firmware would otherwise provide,
namely the `u8g2` object, display state, core's screen and menu state, and the
board macros. u8g2 does no I/O, so `render.cpp` dumps each frame's display
buffer to a `.bin` file. `to_png.py` maps the page-major buffer to pixels and
upscales it (see `OUT_WIDTH` in `to_png.py`).

Because the renderer runs the firmware's own draw code, a rendered frame matches
what the panel shows.

## Frames

The renderer covers every top-level frame, each entered-menu state, and the BLE
versions of the screens that change with the radio mode, so a run produces more
images than there are carousel frames.

## Notes

`render.cpp` sets `U8G2_R0`, so images are upright whatever the rotation of the
device display.

The `ScreenId` and `MenuState` enums in `render.cpp` mirror `core.h`, which
pulls in `Arduino.h` and `esp_wifi.h` and so cannot compile on the host. Update
the mirror when those enums change. The build fails when `screens.inc` names an
enumerator the mirror lacks. An order mismatch builds cleanly and renders the
wrong page number in each header.

Sample values match `DEMO_MODE` in `src/main_oled.cpp` so renders and device
photos agree. Nothing couples the two, so a change to one needs the same change
in the other.
