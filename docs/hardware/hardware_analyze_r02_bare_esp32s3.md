# Analyze r0.2 bare hardware (ESP32-S3)

Built by `env:analyze_r02_n16r8_bare`. The layout is designated r0.4 in the
hardware tree. It keeps every peripheral of the
[r0.2 dev board](hardware_analyze_r02_esp32s3.md) and replaces the DevKitC-1
module with a bare `ESP32-S3-WROOM-1U` N16R8 soldered to the carrier. Read the
r0.2 dev page first, this one covers only the deltas.

## Module and USB

The DevKitC-1 brought its own regulator, USB-to-UART bridge, USB connector and
BOOT and RESET buttons. All four now live on the carrier:

| Function | On the carrier |
|----------|----------------|
| 3V3 rail | AMS1117-3.3 from VBUS, the only regulator on the board |
| USB      | USB-C receptacle wired to the SoC's native USB on GPIO19 and GPIO20 |
| UART0    | 4-pin header, TXD0 and RXD0 with 3V3 and GND |
| RESET    | SW1 to EN, with a 5.1k pull-up |
| BOOT     | SW2 to GPIO0 |

There is no USB-to-UART bridge. The USB-C port enumerates as the SoC's own USB
peripheral, so the env compiles with `ARDUINO_USB_MODE=1` and
`ARDUINO_USB_CDC_ON_BOOT=1` and `Serial` is the USB console. Without those flags
`Serial` would go to UART0 and the USB port would appear dead. The UART0 header
remains available for a bridge cable, which is the route to the ROM bootloader
if native USB is ever unavailable.

Only one regulator now feeds 3V3, which retires the r0.2 dev question of a rail
driven from both ends.

## Button order is reversed

The four-button module connector is wired identically on both boards, but the
GPIOs behind it run the opposite way:

| Module pin | r0.2 dev | r0.2 bare |
|------------|----------|-----------|
| K1, `SW_A` | GPIO39   | GPIO42    |
| K2, `SW_B` | GPIO40   | GPIO41    |
| K3, `SW_C` | GPIO41   | GPIO40    |
| K4, `SW_D` | GPIO42   | GPIO39    |

`board_config.h` orders `BTN_PIN_1` through `BTN_PIN_4` by button rather than by
GPIO, so both boards present the same gestures. The images are not
interchangeable. Flashing the dev image on a bare board reverses every control,
putting Back where Up belongs.

## No serial mirror

GPIO15 is not brought out, so `MIRROR_SERIAL` is 0. Nothing else changes, the
mirror is a debug output with no consumer on the board.
