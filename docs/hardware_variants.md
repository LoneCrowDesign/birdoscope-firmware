# Birdoscope hardware variants

Carrier boards, kits, and full builds are planned for building dedicated drop-in
detectors. Each model shares a common PCB, so you can purchase your own parts
if you started simple and want to upgrade later. If you already own MCUs
or other hardware, you can build on the compatible bare board until you reach
the feature set you want. Code flags enable features as needed.

In the tables below, `x` marks a feature the tier includes and `o` marks one it
does not.

## Detect model

This model focuses on low-cost detection, with limited capability for analysis.

The core of the board is an ESP32-C3 Supermini or XIAO, and each tier adds
functionality. No Detect model has an SD card, but captures still persist across
power cycles in onboard flash (SPIFFS) and can be downloaded from the admin
console web server.

- **Basic**: detection, LED and buzzer alerts, and approximate ranging. No
  screen, GPS, or onboard controls.
- **Plus**: adds a screen and onboard controls to the Basic feature set.
- **Full**: adds GPS for precise ranging estimates. Triangulation is performed
  offline, by exporting captures from the web server.

| bscope_detect | LED/BZ | SCRN | GPS | SD  | CTRL |
|---------------|--------|------|-----|-----|------|
| basic         | x      | o    | o   | o   | o    |
| plus          | x      | x    | o   | o   | x    |
| full          | x      | x    | x   | o   | x    |

## Analyze model

This model focuses on configurability and improved data capture for analytics
workflows. Every tier comes with GPS and expansion pins for custom modification.

The core of the board is an ESP32-S3, either N16R8 or N8R2, with enough exposed
pins to modify the firmware or add peripherals.

- **Basic**: detection and precise ranging, but no SD card. Captures still
  persist in onboard flash.
- **Plus**: adds SD card storage for long-running or multi-session captures.
- **Full**: adds a secondary radio for high-context captures and custom scanning
  modes.

| bscope_analyzer | LED/BZ | SCRN | GPS | SD  | CTRL | RAD2 |
|-----------------|--------|------|-----|-----|------|------|
| basic           | x      | x    | x   | o   | x    | o    |
| plus            | x      | x    | x   | x   | x    | o    |
| full            | x      | x    | x   | x   | x    | x    |

The Analyze r0.1 (ESP32-S3) is the initial carrier board for this model. It
covers the basic and plus tiers and has no second radio. The footprint takes
either an N8R2 or an N16R8 module, so flash `env:analyze_r01_n8r2` or
`env:analyze_r01_n16r8` to match the populated module. See
[Analyze r0.1 hardware](hardware/hardware_analyze_r01_esp32s3.md).

The N16R8 and N8R2 are functionally interchangeable, current code and features
fit comfortably on the N8R2, but the Full tier may require an N16R8 to handle
the logic of a second radio and any onboard analytics. Still in testing.

## Wardriver model (planned)

This model offers maximum onboard functionality and data capture for analytics
and pattern discovery. It aims to combine the capabilities of Joseph Hewitt's
Wardriver r3 and Colonel Panic's flock-you on one device.

## Roadmap

Board revisions, tier consolidation, and planned models are tracked in
[the roadmap](roadmap.md).
