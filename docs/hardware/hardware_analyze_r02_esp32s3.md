# Hardware Details: Birdoscope Analyze r0.2 (ESP32-S3)

Board: Birdoscope Analyze r0.2 carrier board
MCU: ESP32-S3-DevKitC-1 N16R8 module
Schematic: Birdoscope Analyze r0.2, as manufactured
PIO Env: `analyze_r02_n16r8_dev`
Board Config: `include/boards/analyze_r02_esp32s3/board_config.h`
Role: bscope_analyzer basic and plus, see
[`../hardware_variants.md`](../hardware_variants.md)

This is a revision of the Analyze r0.1 board
([`hardware_analyze_r01_esp32s3.md`](hardware_analyze_r01_esp32s3.md)), running
the same firmware (`src/main_oled.cpp`). Read that doc for everything the two
share. This file records only what r0.2 changes.

## Pin changes from r0.1

The board now carries a combined SH1106 plus four-button module replacing the
separate display and 3 buttons. The button row shifts down one GPIO to make
room for the fourth.

| Signal   | r0.1   | r0.2   |
|----------|--------|--------|
| Button 1 | GPIO40 | GPIO39 |
| Button 2 | GPIO41 | GPIO40 |
| Button 3 | GPIO42 | GPIO41 |
| Button 4 | none   | GPIO42 |

Buzzer, WS2812, GPS, SD and the OLED I2C pair all keep their r0.1 GPIOs. The
module 5V pin is no longer fed, since an onboard AMS1117 now derives 3V3 from
VBUS directly.

An r0.1 image on an r0.2 board leaves the first button dead and offsets the
other two, which is why the two revisions get separate envs rather than one
shared config.

## Fourth button

This revision adds a 4th button and replaces the 3 button nav scheme.
The nav scheme for r0.2 uses dedicated forward/back up/down gestures instead
of depending on long presses, etc for overloading.
