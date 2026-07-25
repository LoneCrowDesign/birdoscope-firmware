# Serial debug tools

Small scripts for flashing and bring-up troubleshooting, useful when
`pio device monitor` is not available. In a non-interactive shell it fails with
`termios.error: (25, 'Inappropriate ioctl for device')`.

## Setup

These scripts need `pyserial`, the only host-side dependency in the project.
Install it from the repository root:

```
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

Then run a script with `.venv/bin/python3 tools/serial_debug/<script>.py ...`.

## Scripts

- **`list_ports.py`**: list connected serial devices.
- **`raw_capture.py`**: dump raw bytes from a port for N seconds. Use this when
  a board is not responding to commands and you need to see whether it produces
  any output at all, including ROM bootloader and crash dumps that print before
  the app's `Serial.begin()` takes effect.
- **`send_commands.py`**: send a sequence of word-based serial commands
  (`status`, `dump`, `prev`, `inject`, `log`, `nav <dir>`, `help`) and print the
  responses. Quote any command containing a space, such as `"nav up"`.

## Gotchas

- **Classic ESP32 ROM boot log is 74880 baud, not 115200.** If a classic ESP32
  board, meaning not an S3 or C3, goes silent after a reset, try
  `raw_capture.py --baud 74880` before concluding nothing is being transmitted.
  Reading the ROM's boot banner at the wrong baud produces garbled bytes rather
  than silence, and garbage is easy to misread as no data.
- **Opening a port can itself trigger resets.** pyserial asserts DTR/RTS by
  default on open. A board with an auto-program circuit, where DTR/RTS are wired
  to EN/GPIO0, can boot-loop only while something holds the port open. If a
  board boots fine standalone but loops the instant a monitor attaches, this is
  almost certainly why. Try `--release-dtr-rts` on `raw_capture.py`, or watch the
  board with nothing attached to the port, to isolate whether the monitoring is
  the problem.
- **Two USB ports can mean two different Serial destinations.** A board may
  carry both a hardware UART-to-USB bridge and a native USB-OTG port. The ROM
  bootloader flashes over either, but the running app's `Serial` only reaches
  whichever one matches `ARDUINO_USB_CDC_ON_BOOT`. If a flash succeeds but no
  serial output follows while the board is clearly running, try the other USB
  port before assuming a firmware bug.
