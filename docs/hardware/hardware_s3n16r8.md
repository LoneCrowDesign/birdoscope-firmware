# Hardware Details: ESP32-S3-N16R8

Board: ESP32-S3-N16R8 Devboard (generic)
MCU: ESP32-S3-WROOM-1, 16 MB flash (Quad SPI), 8 MB PSRAM (Octal SPI)
Schematic: `YD-ESP32-S3-SCH-V1.4.pdf`, published by the board vendor
PIO Env: `esp32-s3-devkitc1-n16r8`. The board definition ships with the
pioarduino platform, so the project carries no board JSON of its own
Board Config: `include/boards/esp32s3_devkitc1/board_config.h`, shared with the
N8R2 build
Role: bscope_analyzer plus, see [`../hardware_variants.md`](../hardware_variants.md)

Throughout this document, `*` marks a strapping or otherwise restricted pin.

## Reserved and restricted pins

| GPIO      | Restriction                                                                                           |
|-----------|-------------------------------------------------------------------------------------------------------|
| GPIO26–32 | Internal flash SPI bus, inside the module and not exposed on headers                                  |
| GPIO35–37 | Tied to the octal PSRAM inside the module and unavailable, per the WROOM-1 datasheet                  |
| GPIO33–34 | Free on this module, though ESP-IDF advises avoiding 33–37 in octal configurations                    |
| GPIO0     | Strapping, and the BOOT button. Pulling LOW at reset enters download mode                             |
| GPIO3     | Strapping plus USB-JTAG through an onboard solder bridge. Available if the JTAG bridge is unpopulated |
| GPIO19    | USB2 OTG D−, tied to the native USB-C OTG port. Not usable as GPIO                                    |
| GPIO20    | USB2 OTG D+, tied to the native USB-C OTG port. Not usable as GPIO                                    |
| GPIO43    | UART0 TX, the CH343P bridge to USB1. This is where `Serial` output goes                               |
| GPIO44    | UART0 RX, the CH343P bridge from USB1                                                                 |
| GPIO45    | Strapping, VDD_SPI voltage select. Tie LOW for 3.3 V or leave floating                                |
| GPIO46    | Strapping, ROM log enable. Safe as GPIO after boot when driven by the ESP32                           |
| GPIO48    | Onboard WS2812B RGB LED, also broken out on J2. Driving it as GPIO also drives the LED                |

## USB ports

| Port | Connector | Controller              | Notes                                                    |
|------|-----------|-------------------------|----------------------------------------------------------|
| USB1 | USB-C     | CH343P UART bridge      | Programming and `Serial` I/O, connects to GPIO43/44      |
| USB2 | USB-C     | Native ESP32-S3 USB-OTG | GPIO19 and GPIO20 carry D+ and D−, not available as GPIO |

`Serial` output goes through the CH343P hardware UART bridge rather than
USB-CDC. A hardware UART never blocks on transmit, so no transmit timeout needs
setting.

## Onboard peripherals

| Peripheral        | GPIO    | Notes                                   |
|-------------------|---------|-----------------------------------------|
| RGB LED (WS2812B) | GPIO48  | NEO_GRB + NEO_KHZ800, also on J2 pin 16 |
| Power LED (red)   | n/a     | Fixed, not user-controllable            |
| BOOT button       | GPIO0   | Active LOW, strapping pin               |
| RESET button      | CHIP_PU | Not a GPIO                              |

## Header pinout

J1, 22 pins:

| Pin | Signal  | Pin | Signal  |
|-----|---------|-----|---------|
| 1   | GND     | 12  | GPIO8   |
| 2   | 3V3     | 13  | GPIO3 * |
| 3   | CHIP_PU | 14  | GPIO46  |
| 4   | GPIO4   | 15  | GPIO9   |
| 5   | GPIO5   | 16  | GPIO10  |
| 6   | GPIO6   | 17  | GPIO11  |
| 7   | GPIO7   | 18  | GPIO12  |
| 8   | GPIO15  | 19  | GPIO13  |
| 9   | GPIO16  | 20  | GPIO14  |
| 10  | GPIO17  | 21  | 5V      |
| 11  | GPIO18  | 22  | GND     |

J2, 22 pins:

| Pin | Signal         | Pin | Signal         |
|-----|----------------|-----|----------------|
| 1   | GND            | 12  | GPIO36 * PSRAM |
| 2   | GPIO43 (U0TX)  | 13  | GPIO35 * PSRAM |
| 3   | GPIO44 (U0RX)  | 14  | GPIO0 * BOOT   |
| 4   | GPIO1          | 15  | GPIO45 *       |
| 5   | GPIO2          | 16  | GPIO48 (RGB)   |
| 6   | GPIO42         | 17  | GPIO47         |
| 7   | GPIO41         | 18  | GPIO21         |
| 8   | GPIO40         | 19  | GPIO20 * USB   |
| 9   | GPIO39         | 20  | GPIO19 * USB   |
| 10  | GPIO38         | 21  | GND            |
| 11  | GPIO37 * PSRAM | 22  | GND            |

## Pin assignment

| Signal              | GPIO   | Notes                                                       |
|---------------------|--------|-------------------------------------------------------------|
| NeoPixel (onboard)  | GPIO48 | Onboard WS2812B                                             |
| NeoPixel (external) | GPIO2  | Alternate pixel where the onboard one is unusable. J2 pin 5 |
| BUZZER_PIN          | GPIO4  |                                                             |
| GPS UART RX         | GPIO16 | Serial1, clean GPIO with no conflicts                       |
| GPS UART TX         | GPIO17 | Serial1 TX                                                  |
| OLED SDA            | GPIO8  | Hardware I2C SDA                                            |
| OLED SCL            | GPIO9  | Hardware I2C SCL                                            |
| OLED_RST            | GPIO10 | Or `U8X8_PIN_NONE`                                          |
| SD CS               | GPIO5  |                                                             |
| SD SCK              | GPIO12 | Hardware SPI SCK                                            |
| SD MISO             | GPIO13 | Hardware SPI MISO                                           |
| SD MOSI             | GPIO11 | Hardware SPI MOSI                                           |
| MIRROR_TX           | GPIO15 | Serial2 TX-only, separate from the GPS UART and USB1        |
| BTN_PIN_1           | GPIO1  | Momentary, `INPUT_PULLUP`, to GND. J2 pin 4                 |
| BTN_PIN_2           | GPIO2  | Momentary, `INPUT_PULLUP`, to GND. J2 pin 5                 |

Free GPIOs after the assignment above: GPIO6, GPIO7, GPIO14, GPIO21, GPIO38,
GPIO39, GPIO40, GPIO41, GPIO42, and GPIO47.

## LED

Onboard WS2812B on GPIO48. `LED_PIN` is 48 in
`esp32s3_devkitc1/board_config.h`. GPIO2 is not available as an alternate pixel
here, because this board assigns it to `BTN_PIN_2`.

On the ESP32-S3-DevKitC-1 v1.1 reference design the RGB LED moved to GPIO38.
Some boards keep it on GPIO 48, check there first if your onboard doesn't work.

Colors and timings are in [`../alerts.md`](../alerts.md).

## Buzzer

| GPIO  | Signal | Notes              |
|-------|--------|--------------------|
| GPIO4 | BUZZER | Idle LOW, PWM tone |

Tones and durations are in [`../alerts.md`](../alerts.md).

## Buttons

This board runs the two-button input path, `coreInputTick()`, giving toggle and
mark. See [`../board_parity.md`](../board_parity.md).

| GPIO  | Define      | Notes                             |
|-------|-------------|-----------------------------------|
| GPIO1 | `BTN_PIN_1` | Momentary, `INPUT_PULLUP`, to GND |
| GPIO2 | `BTN_PIN_2` | Momentary, `INPUT_PULLUP`, to GND |

Each button connects its GPIO to GND. Configure `INPUT_PULLUP` and read `LOW` as
pressed. No external resistors are needed.

A double press of BOOT (GPIO0) enters the admin console, as on every board.

## GPS: ATGM336 (GNSS)

The ATGM336H 5-pin breakout exposes VCC, GND, TX, RX, and PPS. The module runs
whenever it is powered.

For antenna selection, ground planes, active against passive and bias, and
reading the `[gps]` serial diagnostic (`chars`, `ok`, `bad`, `fixsent`, `sats`)
to tell an RF problem from a wiring one, see
[`../reference/gps_antennas.md`](../reference/gps_antennas.md).

| GPIO   | Signal   | Direction   | Notes                           |
|--------|----------|-------------|---------------------------------|
| GPIO16 | GNSS_TX  | GPS → ESP32 | Serial1 RX, 9600 baud NMEA      |
| GPIO17 | GNSS_RX  | ESP32 → GPS | Serial1 TX, reserved for config |
| GPIO18 | GNSS_RST | ESP32 → GPS | Active LOW, optional, see below |

`Serial1.begin(9600, SERIAL_8N1, 16, 17)`

RESET is present only on the bare ATGM336H module. The common 5-pin breakout
does not expose it, so leave GPIO18 unconnected in that case.

PPS, a 1 Hz UTC pulse, is available on the breakout and can be wired to any free
GPIO, GPIO14 for example, to give a hardware GPS time reference feeding the
NTP and millis chain.

## OLED (optional)

| GPIO   | Signal   | Notes              |
|--------|----------|--------------------|
| GPIO8  | OLED_SDA | Hardware I2C SDA   |
| GPIO9  | OLED_SCL | Hardware I2C SCL   |
| GPIO10 | OLED_RST | Or `U8X8_PIN_NONE` |

Any 4-pin I2C monochrome OLED wires to the SDA, SCL, and RST pins above, at the
default address 0x3C. The default panel is the SH1106 1.3" 128×64. Match the
u8g2 constructor to the controller before flashing.

| Panel                      | Controller        | u8g2 constructor                      |
|----------------------------|-------------------|---------------------------------------|
| SH1106 1.3" (default)      | SH1106            | `U8G2_SH1106_128X64_NONAME_F_HW_I2C`  |
| 0.96" 128×64               | SSD1306 / SSD1315 | `U8G2_SSD1306_128X64_NONAME_F_HW_I2C` |
| 2.42" 128×64 (largest I2C) | SSD1309           | `U8G2_SSD1306_128X64_NONAME_F_HW_I2C` |

```cpp
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, OLED_RST);
```

The SH1106 has 132 columns of RAM but only 128 visible, so it needs the SH1106
constructor. Driving it with the SSD1306 driver shifts the image about 2 px and
leaves an edge strip of garbage.

The 2.42" SSD1309 keeps the same 4-pin I2C wiring. On those modules, verify the
interface-select resistors (R13, R15, R17) are set for I2C, since many ship
defaulted to SPI.

## microSD card (SPI)

| GPIO   | Signal  |
|--------|---------|
| GPIO5  | SD_CS   |
| GPIO12 | SD_SCK  |
| GPIO13 | SD_MISO |
| GPIO11 | SD_MOSI |

`SPI.begin(12, 13, 11, 5)`

## Serial mirror

| GPIO   | Signal    | Notes                   |
|--------|-----------|-------------------------|
| GPIO15 | MIRROR_TX | Serial2 TX-only, 115200 |

`Serial2.begin(115200, SERIAL_8N1, -1, 15)`

## Memory

| Resource | Size   | Notes                                   |
|----------|--------|-----------------------------------------|
| Flash    | 16 MB  | Quad SPI                                |
| PSRAM    | 8 MB   | Octal, with GPIO35–37 tied to it inside the module |
| SPIFFS   | 3.9 MB | Per `partitions_16mb.csv`, `0x3F0000`        |

Partition table: `partitions_16mb.csv`, with a 12 MB `app0` and a single OTA slot.

The OPI PSRAM needs `memory_type = qio_opi`. That comes from the platform's own
board definition, so the env does not set it.

## PlatformIO env

```ini
[env:esp32-s3-devkitc1-n16r8]
extends = oled
board = esp32-s3-devkitc1-n16r8
build_flags =
    ${common.build_flags}
    -I include/boards/esp32s3_devkitc1
    -DBOARD_HAS_PSRAM
board_build.partitions = partitions_16mb.csv
```

`platform`, `framework`, and `monitor_speed` are inherited from the `[common]`
base, and the library set from `[oled]`. The `esp32-s3-devkitc1-n16r8` board
definition ships with the pioarduino platform, so the project carries no board
JSON of its own and nothing needs placing in a `boards/` directory.

There is no `-DARDUINO_USB_CDC_ON_BOOT` here. `Serial` goes through the CH343P
hardware UART bridge on USB1, not native USB-CDC on USB2, so setting the flag
would initialize the wrong port.

## Sources

- `YD-ESP32-S3-SCH-V1.4.pdf`, published by VCC-GND Studio
- [ESP32-S3 Series Datasheet, Espressif](https://documentation.espressif.com/esp32-s3_datasheet_en.pdf)
- [GPIO & RTC GPIO, ESP-IDF Programming Guide (ESP32-S3)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/gpio.html)
