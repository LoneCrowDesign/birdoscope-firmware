# Birdoscope Firmware

Detection and data capture firmware for locating Flock surveillance
infrastructure. It is intended to allow you to build a dedicated device to
help develop high-confidence position estimates, capture analytic data on
Flock device behaviour, and inform discussions aboutthe deployment of mass
public surveillance.

## What it does

Birdoscope enables automatic Flock detection with several common ESP32 MCUs. It
logs data for visualization and analysis, and allows you to customize various 
features on the fly through the admin web console.

While all basic functionality will work on most ESP dev modules without modification, 
I have designed several integrated carrier boards that shrink the footprint and
make it easy to drop in whatever MCU you have available. These will be available
shortly in the birdoscope-hardware repo.

Detection runs with or without an SD card, captures will be saved to
onboard flash if SD is unavailable. Analytic builds additionally capture GPS, Flock
probe and packet details, nearby access points, and related metadata. Please see
the hardware doc for an explanation of the different models.

This is intended as a feeder device to the birdoscope-analytics app which ingests
capture data and allows you to map observed cameras, look for commonalities, and
identify patterns (among other things).

## How it works

The device scans in flock-you custom mode by default. To retrieve captured data, put it
in Admin mode. It will pause scanning and raise a WiFi access point
(`Birdoscope-Setup`) hosting a small web console at `http://birdoscope.local/`
or `http://10.99.7.1/`. From there you can download the stored session, and any prior logs
on SD-equipped boards.

A board with no onboard controls enters and leaves Admin mode by double-pressing
BOOT at any time. There is no reboot involved, so a GPS fix survives the switch.
To resume detection, use "Return to scan" in the console, double-press BOOT
again, or walk away and let it auto-resume once no client is connected (90s).

On SD-equipped boards you can pull the card to copy over all detection logs.

## Supported hardware

Every board below has a build environment in `platformio.ini`:

| Board | Environment |
|---|---|
| Heltec WiFi LoRa 32 V4 | `heltec_v4` |
| ESP32-S3-DevKitC-1 N16R8 | `esp32-s3-devkitc1-n16r8` |
| ESP32-S3-DevKitC-1 N8R2 | `esp32-s3-devkitc1-n8r2` |
| Birdoscope PCB r0.1, N8R2 module | `pcb_r01_analyze_n8r2` |
| Birdoscope PCB r0.1, N16R8 module | `pcb_r01_analyze_n16r8` |
| ESP32 round LCD (GC9A01) | `esp32round` |

Per-board pinouts live in [`docs/hardware/`](docs/hardware/), and
[`docs/hardware_variants.md`](docs/hardware_variants.md) covers how the variants
differ.

Heltec_v3 should work as well, but is unverified and does not have a carrier
board yet. Please verify pinouts before flashing with the v4 profile, but it
should be fine.

The code is structured to allow you to create a board profile for whatever you
have avaialable by separating the core detection and logging functionality from
hardware-specific constraints.

Just create a board environment in the PIO ini, then copy and customize a 
board_config.h file under 
[`include/boards/<your-board-here>`](include/boards/<your-board-here>). 
Once you've set pin assignments and other persistent config variables, it should
flash without issue when you pio run with the new env as the target.

## Building

Birdoscope builds with [PlatformIO](https://platformio.org/). Pick the
environment matching your board:

```bash
pio run -e esp32-s3-devkitc1-n16r8                 # build
pio run -e esp32-s3-devkitc1-n16r8 -t upload       # build and flash
pio device monitor -b 115200                       # watch serial output
```

Host-side Python tooling, used only by the serial debug scripts, installs
separately:

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

See [`tools/serial_debug/README.md`](tools/serial_debug/README.md) for what
those scripts do and the bring-up gotchas they exist to work around.


## Credit

This project builds on the work of:

- @colonelpanichacks, whose flock-you firmware this is forked from and which
  informed many design decisions
- @NitekryDPaul, for the list of OUI targets, the original
  firmware, and promiscuous-mode detection
- @DeFlockJoplin, for research on the wildcard probe request signatures of Flock
  infrastructure
- @JosephHewitt, for the wardriver_r3 hardware and firmware used for associated
  metadata capture and correlational analysis
- The reporting of @DeFlock.org, @BennJordan, @404Media, @EFF, @Proton, and many
  others covering this topic

[`docs/attribution.md`](docs/attribution.md) maps which parts of the code derive
from upstream work.

## Documentation

- [Supported hardware variants](docs/hardware_variants.md)
- [Per-board pinouts](docs/hardware/)
- [Onboard configuration](docs/onboard_config.md)
- [Detection methods](docs/detection-methods.md): frame semantics, geometry, and
  what each detection type does and does not show
- [Alert behaviour](docs/alerts.md): chirp and flash reference
- [Attribution and provenance](docs/attribution.md)

## License

Original contributions are MIT licensed. Catch as many as you can, however you want.
See [LICENSE](LICENSE).

[NOTICE](NOTICE) records the upstream provenance and one important caveat: the
firmware links the jellybean WebConsole which is GPLv3. A distributed firmware
binary is a combined work governed by GPLv3. The MIT license covers this project's own
source.
