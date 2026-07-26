# Hardware Details: Birdoscope Analyze r0.1 (ESP32-S3)

Board: Birdoscope Analyze r0.1 carrier board
MCU: ESP32-S3. The footprint accepts either an N8R2 or an N16R8 DevKitC-1 module
Schematic: Birdoscope Analyze r0.1, as manufactured
PIO Env: `analyze_r01_n8r2` or `analyze_r01_n16r8`, matching the populated
module
Board Config: `include/boards/analyze_r01_esp32s3/board_config.h`
Role: bscope_analyzer basic and plus, see
[`../hardware_variants.md`](../hardware_variants.md)

Note: this is the productized layout of the ESP32-S3 analyzer developed on the
DevKitC-1 module ([`hardware_s3n16r8.md`](hardware_s3n16r8.md)). It shares that
core and the same firmware (`src/main_oled.cpp`), so it inherits the DevKitC-1
pinout. Read that doc for pins. This file records only what is specific to the
PCB.

## Which module, which env

Firmware and pinout are identical between the two modules, and the on-module
memory settings come entirely from the board definition.

| Module | Flash | PSRAM      | Env                 | Use for                         |
|--------|-------|------------|---------------------|---------------------------------|
| N8R2   | 8 MB  | 2 MB quad  | `analyze_r01_n8r2`  | cheaper, PCB testing            |
| N16R8  | 16 MB | 8 MB octal | `analyze_r01_n16r8` | feature dev, no hardware limits |

The two are not interchangeable at flash time. An N8R2 has an 8 MB chip, so a 16
MB partition table fails at boot. Flash the env matching the populated module.

## What is specific to this PCB

| Item        | Detail                                                                                                                                                                                                                            |
|-------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| OLED driver | SH1106 rather than SSD1306. Needs the SH1106 u8g2 constructor, which applies the 2 px RAM offset                                                                                                                                  |
| OLED mount  | Upside-down, so the framebuffer is rotated 180° (`U8G2_R2`)                                                                                                                                                                       |
| Buttons     | Three onboard on GPIO40, GPIO41, and GPIO42, against two on the DevKitC-1 build. BTN_PIN_2 moved off GPIO2, where it collided with the external LED                                                                               |
| Buzzer      | 12 mm piezo rather than 9 mm. Detection-chirp tones scale down by `(9/12)²`, about 0.56, taking `NEW_CHIRP_*_HZ` from 2000/2800 to 1125/1575. This softens the larger element while preserving the interval between the two notes |
| RGB LED     | External WS2812B on GPIO2 (`LED_PIN=2`), wired to J2 pin 5, rather than the module's onboard GPIO48 pixel                                                                                                                         |

These are set in `include/boards/analyze_r01_esp32s3/board_config.h` via knobs
consumed by the shared `src/main_oled.cpp`:

| Define            | Value     | Effect                                                                                                                                          |
|-------------------|-----------|-------------------------------------------------------------------------------------------------------------------------------------------------|
| `OLED_SH1106`     | `1`       | selects `U8G2_SH1106_128X64_NONAME_F_HW_I2C` instead of the SSD1306 constructor                                                                 |
| `OLED_ROTATION`   | `U8G2_R2` | rotates the display 180°                                                                                                                        |
| `BTN_PIN_3`       | `42`      | third button GPIO, wired through `NAV_SCHEME_3BTN`                                                                                              |
| `NAV_SCHEME_3BTN` | `1`       | runs the three-button semantic nav scheme (`coreNavTick`), giving the carousel and menus, instead of the legacy `coreInputTick` toggle and mark |

The `OLED_SH1106` constructor clears the 2 px column shift that the SSD1306
constructor leaves on this panel.

## Firmware role

Runs the `src/main_oled.cpp` firmware, with a screen, GPS, SD reader, and
controls. The tier does not require SD, though this PCB carries it.
