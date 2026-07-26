# Hardware Details: Heltec WiFi LoRa 32 V4

Board: Heltec WiFi LoRa 32 V4 devboard
MCU: ESP32-S3R2, 16 MB flash, 2 MB PSRAM (quad)
Schematic: [`Heltec4_Datasheet_WiFi_LoRa_32_V4.2.0.pdf`](../reference/Heltec4_Datasheet_WiFi_LoRa_32_V4.2.0.pdf)
PIO Env: `heltec_v4`. The platform publishes no V4 board definition, so this
uses `heltec_wifi_lora_32_V3` and overrides the flash size to 16 MB in
`platformio.ini`. This is the reference Heltec env, and `heltec_v3` derives
from it
Board Config: `include/boards/heltecv4/board_config.h`

Throughout this document, `*` marks a strapping pin.

## Reserved and restricted pins

| GPIO     | Restriction                                                                                     |
|----------|-------------------------------------------------------------------------------------------------|
| GPIO0 *  | Strapping, and the BOOT button (`BOOT_BTN_PIN`). LOW at reset enters download mode              |
| GPIO45 * | Strapping, VDD_SPI voltage select. Pulling it HIGH at boot is destructive, so never wire to it  |
| GPIO46 * | Strapping, ROM log enable. Used as SD_SCK here, see the note under microSD                      |
| GPIO1    | ADC1_CH0, battery voltage sense                                                                 |
| GPIO34   | VGNSS_CTRL, active LOW power enable for the GPS rail                                            |
| GPIO36   | Vext_Ctrl, active LOW enable for the 3.3 V OLED rail                                            |
| GPIO37   | ADC_CTRL, must be HIGH to enable the battery ADC divider                                        |
| GPIO17, GPIO18, GPIO21 | Onboard OLED SDA, SCL, and RST                                                    |

## USB ports

| Port | Connector | Controller              | Notes                                        |
|------|-----------|-------------------------|----------------------------------------------|
| USB1 | USB-C     | Native ESP32-S3 USB-CDC | Sole port, carrying programming and `Serial` |

The env sets `-DARDUINO_USB_CDC_ON_BOOT=1`, so `Serial` is routed over native
USB-CDC. The same connector also feeds the onboard charging path.

## Onboard peripherals

| Peripheral         | GPIO                   | Notes                                       |
|--------------------|------------------------|---------------------------------------------|
| OLED (SSD1315)     | GPIO17, GPIO18, GPIO21 | 0.96 inch, built into the board             |
| Charge status LED  | n/a                    | Fixed white, not user-controllable          |
| Power-on LED       | n/a                    | Fixed orange, not user-controllable         |
| BOOT button        | GPIO0                  | Active LOW, strapping pin                   |
| RESET button       | CHIP_PU                | Not a GPIO                                  |

There is no onboard RGB LED, so the color-coded alerting the firmware expects
needs an external WS2812B.

## Pin assignment

| Signal              | GPIO   | Notes                                          |
|---------------------|--------|------------------------------------------------|
| NeoPixel (external) | GPIO4  | WS2812B, NEO_GRB + NEO_KHZ800, 1 pixel         |
| BUZZER_PIN          | GPIO3  | Passive piezo                                  |
| GPS UART RX         | GPIO39 | Serial2 RX                                     |
| GPS UART TX         | GPIO38 | Serial2 TX                                     |
| GNSS_RST            | GPIO42 | Active LOW                                     |
| GNSS_Wakeup         | GPIO40 |                                                |
| GNSS_PPS            | GPIO41 | Not currently consumed                         |
| VGNSS_CTRL          | GPIO34 | Active LOW power enable for the GPS rail       |
| OLED_SDA            | GPIO17 | Software I2C                                   |
| OLED_SCL            | GPIO18 | Software I2C                                   |
| OLED_RST            | GPIO21 | Active LOW pulse on boot                       |
| Vext_Ctrl           | GPIO36 | Active LOW, enables the 3.3 V OLED rail        |
| SD_CS               | GPIO5  |                                                |
| SD_MOSI             | GPIO6  |                                                |
| SD_MISO             | GPIO2  |                                                |
| SD_SCK              | GPIO46 | Strapping pin, see the note under microSD      |
| MIRROR_TX           | GPIO43 | TX-only at 115200 baud                         |
| VBAT_Read           | GPIO1  | ADC1_CH0                                       |
| ADC_CTRL            | GPIO37 | HIGH to enable the battery ADC divider         |

## LED

| GPIO  | Signal       | Notes                                  |
|-------|--------------|----------------------------------------|
| GPIO4 | NeoPixel DIN | External WS2812B, NEO_GRB + NEO_KHZ800 |

The V4 has no onboard RGB LED, so the pixel is wired to this free GPIO. Colors,
flash duration, and event mapping are in [`../alerts.md`](../alerts.md), with
the values themselves set in `include/boards/heltecv4/board_config.h`. The
notification logic lives in `lib/birdoscope_core/core.cpp`, shared with every
other board.

## Buzzer

| GPIO  | Signal | Notes                                            |
|-------|--------|--------------------------------------------------|
| GPIO3 | BUZZER | Passive piezo, idle LOW, driven with `tone()`    |

Tones and durations are in [`../alerts.md`](../alerts.md).

## Buttons

None. This board sets `HAS_BUTTONS 0` and has no `CTRL` inputs, so BOOT and
RESET are the only onboard controls. A double press of BOOT (GPIO0) enters the
admin console, which is the only way to drive the board without USB.

## GPS: Quectel L76 (GNSS)

Connected through the board's SH1.25-8P connector. Unlike the ATGM336 used on
the other boards, the L76 exposes reset, wakeup, and a switched power rail, and
all three are wired here.

| GPIO   | Signal      | Direction   | Notes                                  |
|--------|-------------|-------------|----------------------------------------|
| GPIO39 | GNSS_TX     | GPS → ESP32 | Serial2 RX, 9600 baud NMEA             |
| GPIO38 | GNSS_RX     | ESP32 → GPS | Serial2 TX, wired but currently unused |
| GPIO42 | GNSS_RST    | ESP32 → GPS | Active LOW, driven HIGH to release     |
| GPIO40 | GNSS_Wakeup | ESP32 → GPS | Driven HIGH on init                    |
| GPIO41 | GNSS_PPS    | GPS → ESP32 | 1 PPS signal, not currently consumed   |
| GPIO34 | VGNSS_CTRL  | ESP32 → PMU | Active LOW power enable for GPS rail   |

Init sequence: assert RST LOW for 10 ms then HIGH, drive Wakeup HIGH, pull
VGNSS_CTRL LOW to power on, then call
`Serial2.begin(9600, SERIAL_8N1, 39, 38)`.

A GPS fix is considered stale after 5000 ms (`GPS_FIX_MAX_AGE_MS`).

## OLED (SSD1315, 0.96 inch)

Built into the board and driven over software I2C (`OLED_HW_I2C 0`).

| GPIO   | Signal    | Notes                                   |
|--------|-----------|-----------------------------------------|
| GPIO17 | OLED_SDA  | I2C data                                |
| GPIO18 | OLED_SCL  | I2C clock                               |
| GPIO21 | OLED_RST  | Reset, active LOW pulse on boot         |
| GPIO36 | Vext_Ctrl | Active LOW, enables the 3.3 V OLED rail |

```cpp
U8G2_SSD1315_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, OLED_SCL, OLED_SDA, OLED_RST);
```

## microSD card (SPI2)

All four pins are on J3. Power the module from the 3.3 V rail and enable it with
`USE_SD 1` once wired.

| GPIO   | Signal  | Notes                             |
|--------|---------|-----------------------------------|
| GPIO5  | SD_CS   | Free GPIO, no conflicts           |
| GPIO6  | SD_MOSI | Free GPIO, no conflicts           |
| GPIO2  | SD_MISO | Free GPIO, no conflicts           |
| GPIO46 | SD_SCK  | Strapping pin, see the note below |

### GPIO46 strapping note

GPIO46 is an ESP32-S3 strapping pin controlling ROM log output, with an internal
weak pull-down. SCK is an ESP32-driven output, so at boot the line holds low,
which samples as ROM logs disabled. No pull-up is needed on a clock line.

Do not use GPIO45 for SD instead: it controls VDD_SPI voltage, and pulling it
high at boot is destructive.

For ROM log output during early-boot debugging, temporarily disconnect the SD
module.

## Serial mirror

| GPIO   | Signal    | Notes                  |
|--------|-----------|------------------------|
| GPIO43 | MIRROR_TX | TX-only at 115200 baud |

Mirrors all Serial output to an external UART adapter, for debugging without USB
CDC.

## Power

| Source            | Connector | Notes                                                    |
|-------------------|-----------|----------------------------------------------------------|
| USB-C             | USB-C     | Powers the board and feeds the charging path             |
| Solar panel       | SH1.25-2P | Pin 1 SOLAR+, pin 2 GND. Straight into the onboard PMU   |
| Lithium battery   | SH1.25-2P | Pin 1 VBAT+, pin 2 GND. PMU manages charge and discharge |

No GPIO is involved in solar input, since the PMU handles MPPT and charging.
Solar and USB may be connected at the same time.

Battery voltage is readable over ADC on GPIO1, with GPIO37 (`ADC_CTRL`) pulled
HIGH to enable the divider. The datasheet gives the scaling as:

```text
VBAT = 100 / (100 + 390) × VADC_IN1
```

The 100 Ω and 390 Ω divider brings battery voltage into the ESP32 ADC input
range.

## Memory

| Resource | Size   | Notes                            |
|----------|--------|----------------------------------|
| Flash    | 16 MB  | Overridden in the env, see below |
| PSRAM    | 2 MB   | Quad, `-DBOARD_HAS_PSRAM`        |
| SPIFFS   | 3.9 MB | Per `partitions.csv`, `0x3F0000` |

Partition table: `partitions.csv`

## PlatformIO env

```ini
[env:heltec_v4]
extends = oled
board = heltec_wifi_lora_32_V3
build_flags =
    ${common.build_flags}
    -I include/boards/heltecv4
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DBOARD_HAS_PSRAM
board_build.flash_size = 16MB
board_upload.flash_size = 16MB
board_upload.maximum_size = 16777216
board_build.partitions = partitions.csv
```

The platform publishes no V4 board definition, so the env borrows the V3 one and
overrides what differs: the V3 is an 8 MB part, the V4 a 16 MB part.
`board_upload.maximum_size` must move with the flash size, or the upload size
check still caps the image at the V3's 8 MB.

This is the reference Heltec env. `heltec_v3` extends it and overrides only the
board config directory and the flash size.

## Sources

- [`Heltec4_Datasheet_WiFi_LoRa_32_V4.2.0.pdf`](../reference/Heltec4_Datasheet_WiFi_LoRa_32_V4.2.0.pdf)
- [ESP32-S3 Series Datasheet, Espressif](https://documentation.espressif.com/esp32-s3_datasheet_en.pdf)
- [GPIO & RTC GPIO, ESP-IDF Programming Guide (ESP32-S3)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/gpio.html)
