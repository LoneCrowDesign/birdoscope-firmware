# Analyze r0.4 Bare Hardware (ESP32-S3)

The `analyze_r04_n16r8_bare` env builds for this board. It keeps every
peripheral of the [r0.4 dev board](hardware_analyze_r04_esp32s3.md) and
replaces the DevKitC-1 module with a bare `ESP32-S3-WROOM-1U` N16R8 soldered to
the carrier. This page covers only the differences from r0.4 dev.

## Module and USB

The DevKitC-1 has its own regulator, USB-to-UART bridge, USB connector and BOOT
and RESET buttons. The bare carrier provides its own versions of these, apart
from the bridge.

| Function | On the carrier |
|----------|----------------|
| 3V3 rail | AMS1117-3.3 from VBUS, the only regulator on the board |
| USB      | USB-C receptacle wired to the SoC's native USB on GPIO19 and GPIO20 |
| UART0    | 4-pin header, TXD0 and RXD0 with 3V3 and GND |
| RESET    | SW1 to EN, with a 5.1k pull-up |
| BOOT     | SW2 to GPIO0 |

The USB-C port enumerates as the SoC's own USB peripheral. The env compiles
with `ARDUINO_USB_MODE=1` and `ARDUINO_USB_CDC_ON_BOOT=1` to make `Serial` the
USB console. Without them `Serial` goes to UART0 and the USB port appears dead.
A bridge cable on the UART0 header reaches the ROM bootloader if native USB
fails.

## Reversed Button Order

Both boards wire the four-button module connector the same way, with the
GPIOs behind it in opposite order.

| Module pin | r0.4 dev | r0.4 bare |
|------------|----------|-----------|
| K1, `SW_A` | GPIO39   | GPIO42    |
| K2, `SW_B` | GPIO40   | GPIO41    |
| K3, `SW_C` | GPIO41   | GPIO40    |
| K4, `SW_D` | GPIO42   | GPIO39    |

`board_config.h` orders `BTN_PIN_1` through `BTN_PIN_4` by button, so both
boards present the same gestures. Flashing the dev image on a bare board
reverses every control, putting Back where Up belongs.

## No Serial Mirror

The carrier leaves GPIO15 unrouted, so `MIRROR_SERIAL` is 0. The mirror is a
debug output, and nothing on the board reads it.
