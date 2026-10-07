# Analyze r0.4 Hardware (ESP32-S3)

- Board: Birdoscope Analyze r0.4 carrier board
- MCU: ESP32-S3-DevKitC-1 N16R8 module
- Schematic: Birdoscope Analyze r0.4, as manufactured
- PIO Env: `analyze_r04_n16r8_dev`
- Board Config: `include/boards/analyze_r04_esp32s3/board_config.h`
- Role: bscope_analyzer basic and plus, see
  [`../hardware_variants.md`](../hardware_variants.md)

r0.4 revises the Analyze r0.1 board
([`hardware_analyze_r01_esp32s3.md`](hardware_analyze_r01_esp32s3.md)) and runs
the same firmware, `src/main_oled.cpp`. This file records only what r0.4
changes.

## Pin Changes from r0.1

A combined SH1106 and four-button module replaces the separate display and
three buttons. The button row shifts down one GPIO to make room for the fourth.

| Signal   | r0.1   | r0.4   |
|----------|--------|--------|
| Button 1 | GPIO40 | GPIO39 |
| Button 2 | GPIO41 | GPIO40 |
| Button 3 | GPIO42 | GPIO41 |
| Button 4 | none   | GPIO42 |

Buzzer, WS2812, GPS, SD and the OLED I2C pair all keep their r0.1 GPIOs. An
onboard AMS1117 derives 3V3 from VBUS, and nothing feeds the module's 5V pin.

An r0.1 image on an r0.4 board leaves the first button dead and offsets the
other two, so each revision has its own env.

## Fourth Button

r0.4 uses `NAV_SCHEME_4BTN`, which gives Back its own button, so Select no
longer needs a long press. Mark stays on a long press of the first button. See
[Menu UX](../menu_ux.md).
