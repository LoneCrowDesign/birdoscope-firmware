# screen_render

Renders the OLED screen carousel to PNGs on the host, with no device attached.

```
./tools/screen_render/render.sh [output-dir]
```

Default output is `working/images/carousel_demo`. Requires `gcc`, `g++`,
`python3` with Pillow, and a populated `.pio/libdeps` (run `pio run -e <env>`
once to fetch u8g2).

## How it works

`render.cpp` includes `src/screens.inc`, the firmware's own drawing code, and
compiles it against u8g2's plain-C sources built natively. It supplies stubs for
everything the firmware would otherwise provide: the `u8g2` object, display
state, core's screen and menu state, and the board macros. u8g2 does no I/O, so
each frame is read straight out of the display buffer and handed to `to_png.py`,
which maps the page-major buffer to pixels and upscales 6x.

Sharing the draw code is the point: a rendered frame cannot drift from what the
panel shows.

## Frames

Covers all top level frames and "entered menu" states so running as-is will
create more images than there are carousel frames.

## Notes

The render uses `U8G2_R0` so images are always upright no matter how the actual
device display is rotated.

The `ScreenId` and `MenuState` enums in `render.cpp` mirror `core.h`, which
cannot be included on the host because it pulls in `Arduino.h` and `esp_wifi.h`.
If those enums change, update the mirror; a drift will mislabel frames rather
than fail the build.

Sample values match `DEMO_MODE` in `src/main_oled.cpp` so renders and device
photos match, but the two are not coupled.
