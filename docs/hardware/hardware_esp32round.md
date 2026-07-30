# Hardware Details: ESP32 round LCD (generic GC9A01 clone)

Board: ESP32-D0WD Round LCD Dev Board (generic)
MCU: ESP32-D0WD, dual-core Xtensa LX6, no PSRAM, 4 MB flash assumed
Schematic: variable by clone. Pinout matches
[a silkscreen-labeled photo of this unit][pinout-photo]
PIO Env: `esp32round`. Uses the generic `esp32dev` board id, with flash size and
pins overridden in `platformio.ini` and `board_config.h`
Board Config: `include/boards/esp32round/board_config.h`

Note: flash size varies between clones. Run `esptool.py --port <port> flash_id`
before the first flash. If the size differs from 4 MB, point
`board_build.partitions` at the table matching the reported size (or add one)
and update `board_build.flash_size` and `board_upload.flash_size` in
`platformio.ini`. Wrong sizes cause a boot loop rather than a clean failure.

Throughout this document, `*` marks a strapping pin.

## Reserved and restricted pins

| GPIO       | Restriction                                                                                     |
|------------|-------------------------------------------------------------------------------------------------|
| IO0 *      | Strapping. LOW at reset enters download mode. `BOOT_BTN_PIN`                                    |
| IO2 *      | Strapping. Used here as the shared SPI MISO, so an inserted SD card can interfere with flashing |
| IO5 *      | Strapping. Used here as TFT CS                                                                  |
| IO12 *     | Strapping, VDD_SPI voltage select. Not used by this build, leave unwired                        |
| IO15 *     | Strapping, boot log enable. Used here as the shared SPI MOSI                                    |
| IO6–IO11   | Internal flash SPI bus, not available as GPIO                                                   |
| IO34–IO39  | Input-only on the classic ESP32. No output drive, and no internal pull-ups                      |

Four of the five classic-ESP32 strapping pins are already committed by this
board's fixed wiring, which is the main constraint on expansion.

## USB ports

| Port | Connector | Controller           | Notes                                    |
|------|-----------|----------------------|------------------------------------------|
| USB1 | USB-C     | Onboard USB-UART bridge | Sole port, carrying programming and `Serial` |

The ESP32-D0WD has no native USB peripheral, so `Serial` goes through the bridge
chip and `-DARDUINO_USB_CDC_ON_BOOT` does not apply to this board.

## Onboard peripherals

| Peripheral        | GPIO | Notes                                              |
|-------------------|------|----------------------------------------------------|
| GC9A01 round TFT  | see below | 240×240, SPI, built into the board            |
| microSD slot      | see below | Shares the SPI bus with the TFT               |
| Top button        | IO19 |                                                    |
| Bottom button     | IO4  |                                                    |
| Power switch      | n/a  | Hardware switch, cuts power rather than driving a GPIO |
| Power LED (green) | n/a  | Fixed, indicates USB power present, not user-controllable |

There is no onboard addressable LED and no buzzer or piezo.

## Header pinout: FPC I/O expansion connector

This board has no pin header. Its only expansion point is an 8-pin FPC/FFC ZIF
ribbon connector, labeled "FPC I/O" in the pinout photo.

| Signal  | GPIO | Notes                                   |
|---------|------|-----------------------------------------|
| U1_TX   | IO26 | UART1 TX                                |
| U1_RX   | IO25 | UART1 RX, wired to the GPS module       |
| IIC_SCL | IO22 |                                         |
| IIC_SDA | IO21 |                                         |
| 3V3     | n/a  |                                         |
| GND     | n/a  |                                         |

The remaining two pins are additional GPIOs; confirm them against the physical
connector before use.

Nothing plugs into an FPC ribbon connector directly, since peripheral modules
ship with 2.54 mm headers or JST-PH pigtails rather than flex tails. An
FPC-to-header breakout adapter converts it to solderable or jumperable pins.

## Pin assignment

| Signal            | GPIO | Notes                                    |
|-------------------|------|------------------------------------------|
| TFT_BL            | IO32 | Backlight                                |
| TFT_CS            | IO5  | Strapping pin                            |
| TFT_DC            | IO27 |                                          |
| TFT_RST           | IO33 |                                          |
| TFT_MOSI / SD CMD | IO15 | Shared SPI bus, strapping pin            |
| TFT_SCLK / SD CLK | IO14 | Shared SPI bus                           |
| TFT_MISO / SD DAT0| IO2  | Shared SPI bus, strapping pin            |
| SD_CS             | IO13 |                                          |
| BTN_PIN_1         | IO19 | Top button, toggles the screen           |
| BTN_PIN_2         | IO4  | Bottom button, manual marker             |
| GPS UART RX       | IO25 | Serial1 RX, on the FPC connector         |
| GPS UART TX       | IO26 | Serial1 TX, on the FPC connector, unused |

Free after the assignment above: IO21 and IO22 on the FPC connector, plus the
two unconfirmed FPC pins.

## LED

None. There is no onboard addressable LED, and no external pixel is wired. The
screen takes over all alerting, so the color-coded events in
[`../alerts.md`](../alerts.md) are rendered rather than flashed.

## Buzzer

None. This board has no onboard buzzer or piezo, so new-detection feedback is
screen-only: a red background and ring pointer on the scan screen.

## Buttons

Two momentary buttons, on the semantic nav layer described in
[`../board_parity.md`](../board_parity.md). Each connects its GPIO to GND,
configured `INPUT_PULLUP` and read `LOW` as pressed, with a 50 ms debounce.

| GPIO | Signal      | Notes                          |
|------|-------------|--------------------------------|
| IO19 | `BTN_PIN_1` | Top button, toggles the screen |
| IO4  | `BTN_PIN_2` | Bottom button, manual marker   |

The middle button is a separate hardware power switch that cuts power directly
rather than through a GPIO. A double press of BOOT (IO0) enters the admin
console, as on every board.

## GPS: ATGM336 (GNSS)

Wired and enabled through the FPC breakout, with `HAS_GPS 1` in
`board_config.h`.

| GPIO | Signal  | Direction   | Notes                             |
|------|---------|-------------|-----------------------------------|
| IO25 | GNSS_TX | GPS → ESP32 | Serial1 RX (`U1_RX`), 9600 baud NMEA |
| IO26 | GNSS_RX | ESP32 → GPS | Serial1 TX (`U1_TX`), unused      |

Leave `RXD` unconnected to save a pin since it is only needed to send config commands.

The ATGM336 has no power, reset, or wakeup control pins, and none are wired. The
module runs always-on from 3V3. Its `PPS` output is also unconnected, and
nothing in firmware reads it.

## Display: GC9A01 240×240 round TFT (SPI)

| Signal          | GPIO | Notes                                                            |
|-----------------|------|------------------------------------------------------------------|
| BLK (backlight) | IO32 | Driven HIGH after init, held LOW during boot to hide boot garbage |
| CS              | IO5  | Strapping pin                                                    |
| DC              | IO27 |                                                                  |
| RES (reset)     | IO33 |                                                                  |
| SDA (MOSI)      | IO15 | Shared SPI bus with the microSD slot                             |
| SCL (SCK)       | IO14 | Shared SPI bus with the microSD slot                             |
| MISO            | IO2  | Shared SPI bus, see the microSD section                          |

Driven by `TFT_eSPI`, configured through `build_flags` in `platformio.ini` at
`SPI_FREQUENCY=20000000`. This is the only board on `src/main_tft.cpp`.

The GC9A01 never reads over SPI, so `TFT_MISO` is not needed for the panel. It
is wired to IO2 anyway so that TFT_eSPI's bus comes up with MISO already
attached for the microSD card sharing it.

The firmware uses a full-screen 240×240 8bpp `TFT_eSprite` back buffer of
57.6 KB. This board has no PSRAM, so that buffer shares SRAM with the WiFi
promiscuous driver.

## microSD card (SPI, shared bus with the TFT)

| Signal       | GPIO | Notes                                                                                            |
|--------------|------|--------------------------------------------------------------------------------------------------|
| DATA3 / CS   | IO13 |                                                                                                  |
| CMD / MOSI   | IO15 | Same pin as TFT SDA, a shared SPI bus                                                            |
| CLK / SCK    | IO14 | Same pin as TFT SCL, a shared SPI bus                                                            |
| DATA0 / MISO | IO2  | Strapping pin. An inserted card can interfere with flashing, so pull the card if an upload fails |
| VDD          | 3V3  |                                                                                                  |
| VSS          | GND  |                                                                                                  |

Both the TFT and SD share one `TFT_eSPI` SPIClass instance, obtained with
`tft.getSPIinstance()`. TFT_eSPI owns a private SPIClass separate from the
global `SPI` object, and a second `SPI.begin()` on these pins re-routes them
through the GPIO matrix and disconnects TFT_eSPI from the bus.

## Serial mirror

None. The would-be mirror pin, IO26 (`U1_TX`), is consumed by the GPS UART pair,
and `MIRROR_SERIAL` is unset in `board_config.h`. The onboard USB-UART bridge's
`Serial` is the only debug and data port.

## Power

| Source                | Notes                                                       |
|-----------------------|-------------------------------------------------------------|
| USB-C                 | Powers the board. The green LED indicates USB power present |
| Hardware power switch | Cuts power independent of any GPIO                          |

There is no documented battery or solar input on this board. Treat it as
USB-tethered, or confirm a separate power rail before field deployment.

## Memory

| Resource | Size          | Notes                                              |
|----------|---------------|----------------------------------------------------|
| Flash    | 4 MB (assumed)| Varies by clone, verify with `esptool.py flash_id` |
| PSRAM    | none          | The TFT back buffer therefore lives in SRAM        |
| SPIFFS   | 2.4 MB        | Per `partitions_4mb.csv`, `0x260000`        |

Partition table: `partitions_4mb.csv`, with a 1.5 MB `app0`. It is sized
for a 4 MB part, so switch tables if `flash_id` reports otherwise.

The `app0` margin here is the tightest in the fleet: the binary is 1.26 MB
against the 1.5 MB slot, leaving roughly 240 KB. The TFT build carries
`TFT_eSPI` and its fonts, which the u8g2 boards do not.

## PlatformIO env

```ini
[env:esp32round]
extends = common
board = esp32dev
build_src_filter = +<main_tft.cpp>
lib_deps =
    ${web.lib_deps}
    bodmer/TFT_eSPI
build_flags =
    ${common.build_flags}
    -I include/boards/esp32round
    -DUSER_SETUP_LOADED=1
    -DGC9A01_DRIVER=1
    ; TFT_eSPI pin and font config, see platformio.ini
board_build.flash_size = 4MB
board_upload.flash_size = 4MB
board_build.partitions = partitions_4mb.csv
```

No `esp32round` board definition exists, so the generic `esp32dev` board id is
used with flash size overridden. This env extends `[common]` directly rather
than `[oled]`, because it builds `src/main_tft.cpp` and pulls in `TFT_eSPI`
instead of u8g2. The TFT_eSPI pin config is compiled in through `build_flags`
rather than a `User_Setup.h`, so the pins above are set in `platformio.ini`
rather than `board_config.h`.

## Sources

- [a silkscreen-labeled photo of this unit][pinout-photo]
- [GPIO & RTC GPIO, ESP-IDF Programming Guide (ESP32)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/gpio.html)
- [ESP32 Boot Mode Selection, esptool documentation](https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/boot-mode-selection.html)

[pinout-photo]: https://content.instructables.com/FEO/K5GO/M1GG5EXU/FEOK5GOM1GG5EXU.png
