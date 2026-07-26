# Hardware pinout: Seeed XIAO ESP32-S3

Board: Seeed XIAO ESP32-S3 devboard, standard or Sense
MCU: ESP32-S3, 8 MB flash, 8 MB PSRAM
Schematic: [Seeed XIAO ESP32-S3 getting started](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
PIO Env: `xiao_esp32s3`, using the `seeed_xiao_esp32s3` board id
Board Config: `include/boards/xiao_esp32s3/board_config.h`
Role: bscope_detect, see [`../hardware_variants.md`](../hardware_variants.md)

Note: the pin assignments below have not been confirmed against a wired board.
Verify them against the silkscreen before connecting hardware.

## Available GPIO

The standard and Sense variants expose 13 user-accessible pins.

| XIAO Label | GPIO   | Default function | Notes                                    |
|------------|--------|------------------|------------------------------------------|
| D0         | GPIO1  | ADC/Touch        |                                          |
| D1         | GPIO2  | ADC/Touch        |                                          |
| D2         | GPIO3  | ADC/Touch        |                                          |
| D3         | GPIO4  | ADC/Touch        |                                          |
| D4         | GPIO5  | I2C SDA          |                                          |
| D5         | GPIO6  | I2C SCL          |                                          |
| D6         | GPIO43 | UART TX          |                                          |
| D7         | GPIO44 | UART RX          |                                          |
| D8         | GPIO7  | SPI SCK          |                                          |
| D9         | GPIO8  | SPI MISO         |                                          |
| D10        | GPIO9  | SPI MOSI         |                                          |
| D11        | GPIO42 | GPIO             |                                          |
| D12        | GPIO41 | GPIO             | ADC not supported per datasheet          |
| n/a        | GPIO21 | Onboard LED      | Single-color, active LOW, not a NeoPixel |

The XIAO Plus adds five more GPIOs: GPIO38, GPIO13, GPIO12, GPIO11, and GPIO10.

Any GPIO not in the table above, including GPIO34, GPIO36, and GPIO40, is not
broken out on this form factor and cannot be used.

## USB ports

| Port | Connector | Controller              | Notes                                        |
|------|-----------|-------------------------|----------------------------------------------|
| USB1 | USB-C     | Native ESP32-S3 USB-CDC | Sole port, carrying programming and `Serial` |

The env sets `-DARDUINO_USB_CDC_ON_BOOT=1`, so `Serial` is routed over native
USB-CDC. This is what keeps `Serial` working while GPS consumes the only
hardware UART pair.

## Onboard peripherals

| Peripheral   | GPIO    | Notes                                      |
|--------------|---------|--------------------------------------------|
| User LED     | GPIO21  | Single-color yellow, active LOW            |
| BOOT button  | GPIO0   | Active LOW, not broken out on the header   |
| RESET button | n/a     | Not a GPIO                                 |

There is no onboard display, RGB LED, SD slot, or buzzer. Every peripheral in
this document is external.

## Pin assignment

The XIAO suits the Detect model, so only the Detect signals are assigned. The
`Tier` column gives which of Basic, Plus, and Full each one belongs to, per
[`../hardware_variants.md`](../hardware_variants.md).

| Signal             | XIAO Pin | GPIO   | Tier       | Notes                                                           |
|--------------------|----------|--------|------------|-----------------------------------------------------------------|
| LED_PIN (NeoPixel) | D12      | GPIO41 | all        | External WS2812B required                                       |
| BUZZER_PIN         | D3       | GPIO4  | all        |                                                                 |
| OLED SDA           | D4       | GPIO5  | Plus, Full | Hardware I2C                                                    |
| OLED SCL           | D5       | GPIO6  | Plus, Full | Hardware I2C                                                    |
| OLED RST           | n/a      | n/a    | Plus, Full | Uses `U8X8_PIN_NONE`, since the onboard LED is not a reset line |
| GPS UART RX        | D7       | GPIO44 | Full       | Serial1, the only available UART pair                           |
| GPS UART TX        | D6       | GPIO43 | Full       | Serial1 TX                                                      |

Detect has no SD card at any tier, since captures live in SPIFFS and come off
over the web server. There is no serial mirror either, because GPS consumes the
only UART pair.

Free after Full, the largest tier: GPIO1, GPIO2, GPIO3, GPIO7, GPIO8, GPIO9,
and GPIO42. That is 7 spare, since the ATGM336 needs no control pins.

Plus and Full both include `CTRL`, four buttons on the fleet's standard scheme,
which fit in the free pins above.

## LED

| GPIO   | Signal       | Notes                                       |
|--------|--------------|---------------------------------------------|
| GPIO41 | NeoPixel DIN | External WS2812B on D12, `LED_PIN`          |
| GPIO21 | User LED     | Onboard, single-color yellow, active LOW    |

The XIAO has no onboard RGB LED, so an external WS2812B on D12 provides the
color feedback the firmware expects. Colors and timings are in
[`../alerts.md`](../alerts.md).

The onboard GPIO21 LED is single color and active LOW, so it cannot render the
color-coded events. It is left available for a plain heartbeat or boot-alive
blink.

## Buzzer

| GPIO  | Signal | Notes                                    |
|-------|--------|------------------------------------------|
| GPIO4 | BUZZER | External passive piezo on D3, idle LOW   |

Tones are in [`../alerts.md`](../alerts.md).

## Buttons

The XIAO has no onboard navigation buttons, so Plus and Full take four external
momentary buttons on free GPIOs. Each connects its GPIO to GND, configured
`INPUT_PULLUP` and read `LOW` as pressed. Seven GPIOs are free, so all four fit.

Basic has no `CTRL`. On that tier a double press of BOOT is the only onboard
input, and it enters the admin console. BOOT works the same way on every tier,
though it is not broken out on the XIAO header.

## GPS: ATGM336 (GNSS)

Uses the XIAO's only exposed hardware UART pair, D6 and D7, configured as
`Serial1`. The ATGM336H 5-pin breakout exposes only VCC, GND, TX, RX, and PPS,
so there is no reset, wakeup, or power-enable line. The module runs whenever it
is powered.

| XIAO Pin | GPIO   | Signal  | Direction   | Notes                           |
|----------|--------|---------|-------------|---------------------------------|
| D7       | GPIO44 | GNSS_TX | GPS → ESP32 | Serial1 RX, 9600 baud NMEA      |
| D6       | GPIO43 | GNSS_RX | ESP32 → GPS | Serial1 TX, reserved for config |

`Serial1.begin(9600, SERIAL_8N1, 44, 43)`

PPS is available on the breakout and can be wired to any free GPIO if a hardware
time reference is wanted later. Nothing in firmware reads it today.

Because GPS consumes the UART pair, the serial mirror must be disabled with
`MIRROR_SERIAL 0`.

## OLED (Plus, Full)

| XIAO Pin | GPIO  | Signal   | Notes                                    |
|----------|-------|----------|------------------------------------------|
| D4       | GPIO5 | OLED_SDA | Hardware I2C SDA                         |
| D5       | GPIO6 | OLED_SCL | Hardware I2C SCL                         |


This module is 4-pin, VCC/GND/SCK/SDA. It has no RST line, so the u8g2
constructor takes `U8X8_PIN_NONE` for reset.

I2C runs on the XIAO's hardware pins, so the constructor is the `_HW_I2C` variant.

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

## Power

| Source        | XIAO pin | Notes                                     |
|---------------|----------|-------------------------------------------|
| USB 5V in/out | 5V       | Powers the board from USB-C               |
| 3.3V out      | 3V3      | Regulated 700 mA, powers the OLED and GPS |
| Ground        | GND      |                                           |

There is no onboard battery connector or solar input, so field deployment
requires external battery management.

## Memory

| Resource | Size   | Notes                                    |
|----------|--------|------------------------------------------|
| Flash    | 8 MB   | Set in the env                           |
| PSRAM    | 8 MB   | `-DBOARD_HAS_PSRAM`                      |
| SPIFFS   | 3.9 MB | Per `partitions_8mb.csv`, `0x3F0000`     |

Partition table: `partitions_8mb.csv`, the 8 MB layout with a 4 MB `app0` and a
single OTA slot.

Captures persist in SPIFFS and come off over the web server, as on every Detect
board.

## PlatformIO env

```ini
[env:xiao_esp32s3]
extends = oled
board = seeed_xiao_esp32s3
build_flags =
    ${common.build_flags}
    -I include/boards/xiao_esp32s3
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DBOARD_HAS_PSRAM
board_build.flash_size = 8MB
board_upload.flash_size = 8MB
board_build.partitions = partitions_8mb.csv
```

The `seeed_xiao_esp32s3` board definition ships with the platform, so the
project carries no board JSON of its own.

## Sources

- [Seeed XIAO ESP32-S3 getting started](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
