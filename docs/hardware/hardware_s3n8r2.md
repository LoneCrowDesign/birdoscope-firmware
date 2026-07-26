# Hardware Details: ESP32-S3-N8R2

Board: ESP32-S3-N8R2 Devboard (generic)
MCU: ESP32-S3-WROOM-1, 8 MB flash (Quad SPI), 2 MB PSRAM (Quad SPI)
Schematic: Pull from vendor, varies by manufacturer
PIO Env: `esp32-s3-devkitc1-n8r2`
Board Config: `include/boards/esp32s3_devkitc1/board_config.h`
Role: bscope_analyzer models, see
[`../hardware_variants.md`](../hardware_variants.md)

Note: this module differs from the N16R8
([`hardware_s3n16r8.md`](hardware_s3n16r8.md)) only in on-module memory. The
pinout, peripheral set, and behavior come from that doc so read it for pins.
This file records only what differs.

## What differs from the N16R8

| Property      | N16R8                | N8R2 (this board) |
|---------------|----------------------|-------------------|
| Flash         | 16 MB (Quad SPI)     | 8 MB (Quad SPI)   |
| PSRAM         | 8 MB octal (OPI)     | 2 MB quad (QSPI)  |
| Pinout        | see the N16R8 doc    | identical         |
| Firmware role | bscope_analyzer      | same              |

Everything else is identical and comes from
`include/boards/esp32s3_devkitc1/board_config.h`, which this env reuses verbatim
through `-I include/boards/esp32s3_devkitc1`. That covers GPS, OLED, SD,
buttons, RGB LED, buzzer, the CH343P hardware-UART `Serial` path, and the
channel plan.

## Build differences

No code changes are involved. The memory difference lives entirely in the board
definition rather than in per-env overrides. Both definitions ship with the
pioarduino platform, so the project carries no board JSON of its own, and
`board = esp32-s3-devkitc1-n8r2` resolves against the platform.

| Field         | N16R8         | N8R2         |
|---------------|---------------|--------------|
| `flash_size`  | `16MB`        | `8MB`        |
| `memory_type` | `qio_opi`     | `qio_qspi`   |
| `psram_type`  | `opi` (octal) | `qio` (quad) |

R2 PSRAM is quad rather than octal, so a `qio_opi` build fails PSRAM init at
boot.

The env sets the app partition table, `partitions_8mb.csv`, an 8 MB layout with
a 4 MB app0, spiffs at `0x3F0000`, and a single OTA slot. It otherwise reuses
the shared DevKitC-1 pinout.

## Tradeoffs

The current feature set fits the N8R2 with headroom to spare. The binary is
1.24 MB against a 4 MB app0 partition, and no application code allocates PSRAM
in bulk. A second radio or onboard analytics may require the N16R8.

## The freed PSRAM pins

On the N16R8, GPIO35 through GPIO37 are tied to the octal PSRAM inside the
module and are unavailable. The N8R2's quad PSRAM shares the flash SPI bus
instead, so those three GPIOs are free on this module.