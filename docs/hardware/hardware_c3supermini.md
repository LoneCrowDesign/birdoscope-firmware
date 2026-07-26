# Hardware Details: ESP32-C3 Supermini

Board: ESP32-C3 Supermini Devboard
MCU: ESP32-C3 (single-core RISC-V, no PSRAM), integrated 4 MB flash
Schematic: none published per clone. Pin functions are chip-level, from the
ESP32-C3 datasheet (see Sources section)
PIO Env: none official in `platform-espressif32`. Use the generic
`esp32-c3-devkitm-1` board id and override flash size (see PlatformIO env
below).
Board Config: `include/boards/esp32c3supermini/board_config.h`
Role: bscope_detect, all three tiers, see
[`../hardware_variants.md`](../hardware_variants.md)

Note: "Supermini" is a generic clone form factor produced by many manufacturers
with no authoritative schematic. Pin functions below are tagged to the
ESP32-C3 die, but devboard pinouts vary. Verify against the silkscreen on the
actual board before connecting to a carrier board.

Throughout this document, `*` marks a strapping pin.

## Reserved and restricted pins

| GPIO      | Restriction                                                                                                                                                                                                        |
|-----------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| GPIO2     | Strapping. Must be HIGH, or floating with the default pull-up, at reset. Also ADC1_CH2. Used as BTN_D on Plus and Full (see Buttons), the same accepted risk profile as GPIO9/BOOT: do not hold it during power-up |
| GPIO8     | Strapping. Must be HIGH at reset. Onboard status LED, active LOW, single color rather than addressable                                                                                                             |
| GPIO9     | Strapping. BOOT mode select, internal pull-up. LOW at reset selects UART download mode. Onboard BOOT button                                                                                                        |
| GPIO11–17 | Internal flash SPI bus (4 MB integrated flash), not exposed on the header                                                                                                                                          |
| GPIO18    | Native USB D−, internal, not exposed on the header                                                                                                                                                                 |
| GPIO19    | Native USB D+, internal, not exposed on the header                                                                                                                                                                 |
| GPIO4–7   | Default JTAG (MTMS/MTDI/MTCK/MTDO), free to use as GPIO since the firmware does not use JTAG                                                                                                                       |
| GPIO20    | UART0 RX, the default hardware console UART. Free if `Serial` is routed over native USB-CDC instead                                                                                                                |
| GPIO21    | UART0 TX, as above                                                                                                                                                                                                 |
| GPIO5     | ADC2_CH0. ADC2 readings are unreliable or unavailable while WiFi is active, though digital I/O on this pin is unaffected                                                                                           |

Exposed header GPIOs, 13 in total: 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 20, 21.
GPIO11 through GPIO19 exist on the die but are not broken out on this form
factor.

## USB ports

| Port | Connector | Controller                                    | Notes                                                 |
|------|-----------|-----------------------------------------------|-------------------------------------------------------|
| USB1 | USB-C     | Native ESP32-C3 USB-CDC (GPIO18/19, internal) | Sole port, carrying both programming and `Serial` I/O |

There is no secondary UART-bridge chip, so `Serial` requires
`-DARDUINO_USB_CDC_ON_BOOT=1` in `build_flags`. Without that flag
`Serial.begin()` targets UART0 (GPIO20/21) instead, which is not wired to the
USB port on this board and appears as "no serial output".

## Onboard peripherals

| Peripheral   | GPIO    | Notes                                                           |
|--------------|---------|-----------------------------------------------------------------|
| Status LED   | GPIO8   | Single color, active LOW, not addressable. Also a strapping pin |
| BOOT button  | GPIO9   | Active LOW, strapping pin, internal pull-up                     |
| RESET button | CHIP_PU | Not a GPIO                                                      |

There is no onboard WS2812B addressable LED on this form factor. The color-coded
event table below requires an external WS2812B wired to a free GPIO.

## Header pinout

Two rows of 8 pins, 16 in total. Silkscreen order is clone-dependent, so confirm
against the physical board before laying out the PCB footprint. The commonly
seen ordering is below.

Left header, top to bottom, viewed from the back:

| Pin | Signal        |
|-----|---------------|
| 1   | 5V            |
| 2   | GND           |
| 3   | 3V3           |
| 4   | GPIO4         |
| 5   | GPIO3         |
| 6   | GPIO2 * BTN_D |
| 7   | GPIO1         |
| 8   | GPIO0         |

Right header, top to bottom, viewed from the back:

| Pin | Signal       |
|-----|--------------|
| 1   | GPIO5        |
| 2   | GPIO6        |
| 3   | GPIO7        |
| 4   | GPIO8 * LED  |
| 5   | GPIO9 * BOOT |
| 6   | GPIO10       |
| 7   | GPIO20       |
| 8   | GPIO21       |

## Pin assignment

Detect tiers do not include SD, since captures are stored in a SPIFFS log ring.
See [`../hardware_variants.md`](../hardware_variants.md) for explanations of
features.

### Pin assignment: Detect Basic

| Signal              | GPIO  | Notes                                                           |
|---------------------|-------|-----------------------------------------------------------------|
| NeoPixel (external) | GPIO4 | WS2812B, avoids strapping pins 2, 8, and 9                      |
| BUZZER_PIN          | GPIO5 | PWM tone. Digital use is unaffected by GPIO5's ADC2/WiFi caveat |

Basic has no screen, GPS, SD, or controls, only LED and buzzer alerting with
RSSI ranging.

Free GPIOs after Basic: 0, 1, 2 *, 3, 6, 7, 10, 20, 21. That is 9 spare, one of
them strapping-restricted.

### Pin assignment: Detect Plus

Adds a screen and onboard controls to Basic.

| Signal              | GPIO    | Notes                                                                            |
|---------------------|---------|----------------------------------------------------------------------------------|
| NeoPixel (external) | GPIO4   | Same as Basic                                                                    |
| BUZZER_PIN          | GPIO5   | Same as Basic                                                                    |
| OLED_SDA            | GPIO6   | I2C SDA. ESP32-C3 I2C is fully GPIO-matrix routable, with no fixed hardware pins |
| OLED_SCL            | GPIO7   | I2C SCL                                                                          |
| BTN_A               | GPIO0   | Momentary, `INPUT_PULLUP`, to GND                                                |
| BTN_B               | GPIO1   | Momentary, `INPUT_PULLUP`, to GND                                                |
| BTN_C               | GPIO3   | Momentary, `INPUT_PULLUP`, to GND                                                |
| BTN_D               | GPIO2 * | Momentary, `INPUT_PULLUP`, to GND. Strapping pin, see the caveat in Buttons      |

### Pin assignment: Detect Full

Adds GPS for precise ranging on top of Plus.

| Signal              | GPIO    | Notes                                               |
|---------------------|---------|-----------------------------------------------------|
| NeoPixel (external) | GPIO4   | Same as Basic and Plus                              |
| BUZZER_PIN          | GPIO5   | Same as Basic and Plus                              |
| OLED_SDA            | GPIO6   | Same as Plus                                        |
| OLED_SCL            | GPIO7   | Same as Plus                                        |
| BTN_A               | GPIO0   | Same as Plus                                        |
| BTN_B               | GPIO1   | Same as Plus                                        |
| BTN_C               | GPIO3   | Same as Plus                                        |
| BTN_D               | GPIO2 * | Same as Plus                                        |
| GPS UART RX         | GPIO20  | Serial1 RX, shared with the UART0 console, see note |
| GPS UART TX         | GPIO21  | Serial1 TX, shared with the UART0 console, see note |

GPIO20 and GPIO21 double as the default UART0 console pins, but since `Serial`
is routed over native USB-CDC those two pins are free for `Serial1` and GPS use
without conflict.

Full uses 10 of the 11 free header GPIOs.

## LED

External WS2812B on GPIO4 on all tiers. Colors and timings are in
[`../alerts.md`](../alerts.md).

The onboard GPIO8 status LED is single color and active LOW, so it cannot render
the color-coded events. It is left available for a plain heartbeat or boot-alive
blink independent of those events.

## Buzzer

| GPIO  | Signal | Notes              |
|-------|--------|--------------------|
| GPIO5 | BUZZER | Idle LOW, PWM tone |

## Buttons (Plus, Full)

Four momentary buttons give onboard control matching the Analyze models, with
the same interaction model as the rest of the fleet: up, down, select, and back.
Each button connects its GPIO to GND. Configure `INPUT_PULLUP` and read `LOW` as
pressed.

| GPIO    | Signal | Notes                                                              |
|---------|--------|--------------------------------------------------------------------|
| GPIO0   | BTN_A  | Previous or up                                                     |
| GPIO1   | BTN_B  | Next or down                                                       |
| GPIO3   | BTN_C  | Select or action                                                   |
| GPIO2 * | BTN_D  | Strapping pin, do not hold during power-up (see Reserved pins) |

Buttons are not present on Basic, which has no `CTRL` per
[`../hardware_variants.md`](../hardware_variants.md). A double press of BOOT
(GPIO9) enters the admin console on every tier, and on Basic it is the only
onboard input.

## GPS: ATGM336 GNSS (Full only)

The ATGM336H 5-pin breakout exposes VCC, GND, TX, RX, and PPS.

| GPIO   | Signal  | Direction   | Notes                           |
|--------|---------|-------------|---------------------------------|
| GPIO20 | GNSS_TX | GPS → ESP32 | Serial1 RX, 9600 baud NMEA      |
| GPIO21 | GNSS_RX | ESP32 → GPS | Serial1 TX, reserved for config |

`Serial1.begin(9600, SERIAL_8N1, 20, 21)`

PPS is not wired by default. GPIO10 is spare on Full if PPS is wanted later.

## OLED (Plus, Full)

| GPIO  | Signal   | Notes                                 |
|-------|----------|---------------------------------------|
| GPIO6 | OLED_SDA | I2C SDA, software-routed, GPIO matrix |
| GPIO7 | OLED_SCL | I2C SCL                               |

This module is 4-pin, VCC/GND/SCK/SDA. It has no RST line, so the u8g2
constructor takes `U8X8_PIN_NONE` for reset.

The default panel is the SH1106 1.3" 128×64. Match the u8g2 constructor to the
controller before flashing.

| Panel                      | Controller        | u8g2 constructor                      |
|----------------------------|-------------------|---------------------------------------|
| SH1106 1.3" (default)      | SH1106            | `U8G2_SH1106_128X64_NONAME_F_HW_I2C`  |
| 0.96" 128×64               | SSD1306 / SSD1315 | `U8G2_SSD1306_128X64_NONAME_F_HW_I2C` |
| 2.42" 128×64 (largest I2C) | SSD1309           | `U8G2_SSD1306_128X64_NONAME_F_HW_I2C` |

```cpp
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
```

The SH1106 has 132 columns of RAM but only 128 visible, so it needs the SH1106
constructor. Driving it with the SSD1306 driver shifts the image about 2 px and
leaves an edge strip of garbage.

## Serial mirror

None. Diagnostics and logging are served over the same SoftAP on-demand web
server already used for capture downloads (per
[`../hardware_variants.md`](../hardware_variants.md)), a pseudo-serial log
endpoint rather than a wired UART. Use the web portal for advanced logging and
config.

## Memory

| Resource        | Size             | Notes                                                                                                                                                                              |
|-----------------|------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Flash           | 4 MB             | Integrated in the ESP32-C3 module, not a separate SPI chip on the header                                                                                                           |
| PSRAM           | none             | The ESP32-C3 has no PSRAM support at all, so there is no `BOARD_HAS_PSRAM` flag on this env                                                                                        |
| SPIFFS/LittleFS | ~1–1.5 MB (est.) | Needs a partition table sized for 4 MB total flash, far smaller than the S3N16R8's 16 MB flash and 3.9 MB SPIFFS. Used for the on-demand capture web server, not long-term storage |

No Detect tier has an SD card. Captures live in flash and RAM only, downloaded
via the on-demand web server per
[`../hardware_variants.md`](../hardware_variants.md).

## PlatformIO env

[env:esp32-c3-supermini]

No official `esp32-c3-supermini` board JSON exists in `platform-espressif32`,
but `esp32-c3-devkitm-1` is chip-identical and used as the closest match, with
flash size and pins overridden in firmware. This is the same approach used for
the `esp32round` env in this project's `platformio.ini`, which uses the generic
`esp32dev` board id.

## Sources

- [ESP32-C3 Super Mini Pinout Reference, Last Minute Engineers](https://lastminuteengineers.com/esp32-c3-super-mini-pinout-reference/)
- [Getting Started with the ESP32-C3 Super Mini, Random Nerd Tutorials](https://randomnerdtutorials.com/getting-started-esp32-c3-super-mini/)
- [Notes on ESP32-C3 GPIO: Strapping Pins, Flash Pins, etc, PCBArtists](https://pcbartists.com/design/embedded/esp32-c3-gpio-notes-strapping-pins/)
- [GPIO & RTC GPIO, ESP-IDF Programming Guide (ESP32-C3)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/peripherals/gpio.html)
