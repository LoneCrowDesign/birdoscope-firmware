# Birdoscope Hardware Variants

Birdoscope will offer carrier boards, kits and full builds for dedicated drop-in
detectors. Every tier of a model shares one PCB, so a simple build can
take parts later to move up a tier. If you already own MCUs or other hardware,
you can build on the compatible bare board until you reach the feature set you
want. Build flags enable features as needed.

In the tables below, `x` marks a feature the tier includes and `o` marks one it
does not.

## Detect Model

This model focuses on low-cost detection, with limited capability for analysis.

An ESP32-C3 Supermini or XIAO runs every tier, and each tier adds
functionality. No Detect model has an SD card. Captures persist across power
cycles in onboard flash (SPIFFS), and the admin console serves them for
download.

- **Basic**: detection, LED and buzzer alerts, and approximate ranging. No
  screen, GPS, or onboard controls.
- **Plus**: adds a screen and onboard controls to the Basic feature set.
- **Full**: adds GPS for precise ranging estimates. Triangulation runs offline,
  on captures exported from the admin console.

| bscope_detect | LED/BZ | SCRN | GPS | SD  | CTRL |
|---------------|--------|------|-----|-----|------|
| basic         | x      | o    | o   | o   | o    |
| plus          | x      | x    | o   | o   | x    |
| full          | x      | x    | x   | o   | x    |

## Analyze Model

This model focuses on configurability and improved data capture for analytics
workflows. Every tier comes with GPS and expansion pins for custom modification.

An ESP32-S3, either N16R8 or N8R2, runs every tier, with enough exposed pins to
modify the firmware or add peripherals.

- **Basic**: detection and precise ranging, with captures in onboard flash and
  no SD card.
- **Plus**: adds SD card storage for long-running or multi-session captures.
- **Full**: adds a secondary radio for high-context captures and custom scanning
  modes.

| bscope_analyzer | LED/BZ | SCRN | GPS | SD  | CTRL | RAD2 |
|-----------------|--------|------|-----|-----|------|------|
| basic           | x      | x    | x   | o   | x    | o    |
| plus            | x      | x    | x   | x   | x    | o    |
| full            | x      | x    | x   | x   | x    | x    |

The Analyze r0.1 (ESP32-S3) is the prototype carrier board for this model. It
covers the basic and plus tiers and has no second radio. The footprint takes
either an N8R2 or an N16R8 module, so flash the `analyze_r01_n8r2` or
`analyze_r01_n16r8` env to match the populated module. See
[Analyze r0.1 hardware](hardware/hardware_analyze_r01_esp32s3.md).

The Analyze r0.4 revises r0.1 and comes in two versions, one taking a DevKitC-1
module like the r0.1 and one with a bare ESP32-S3-WROOM-1U soldered on.

The `analyze_r04_n16r8_dev` env builds the DevKitC-1 version for an N16R8
module. The board also takes an N8R2, which has no r0.4 env yet. See
[Analyze r0.4 hardware](hardware/hardware_analyze_r04_esp32s3.md).

The `analyze_r04_n16r8_bare` env builds the bare ESP32-S3-WROOM-1U version.
See [Analyze r0.4 bare hardware](hardware/hardware_analyze_r04_bare_esp32s3.md).

The current firmware fits comfortably on an N8R2. The Full tier may need an
N16R8 for a second radio and onboard analytics, which is still in testing.

## Wardriver Model (Planned)

This model offers maximum onboard functionality and data capture for analytics
and pattern discovery.

## Roadmap

[The roadmap](roadmap.md) tracks board revisions, tier consolidation and planned
models.
