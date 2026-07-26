# Roadmap

Planned hardware and firmware work. For what exists today, see
[hardware variants](hardware_variants.md).

## Board revisions

| Board                     | Status                         |
|---------------------------|--------------------------------|
| Analyze r0.1 (ESP32-S3)   | Current revision               |
| Analyze r0.2 (ESP32-S3)   | Releasing shortly              |
| Detect carrier (ESP32-C3) | Pinout planned, not fabricated |

The Analyze r0.1 covers the basic and plus tiers and has no second radio. Its
pinout is documented in
[Analyze r0.1 hardware](hardware/hardware_analyze_r01_esp32s3.md).

The Detect carrier pinout is planned against the ESP32-C3 Supermini, with tier
assignments in
[ESP32-C3 Supermini hardware](hardware/hardware_c3supermini.md). No board
config or build environment exists for it yet.

## Tier consolidation

All of this is still undergoing real-world testing and may get collapsed into
three standardized tiers for simplicity. In the meantime, the intent is to let
you spin up a detector from whatever hardware you have on hand, at a flexible
price point.

## Models

The Wardriver model is planned but not started. It aims to combine the
capabilities of Joseph Hewitt's Wardriver r3 and Colonel Panic's flock-you on
one device.

The Full Analyze tier depends on a secondary radio, which may require an N16R8
module rather than an N8R2 to carry the additional logic and any onboard
analytics.