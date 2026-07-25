#!/usr/bin/env python3
"""Raw serial capture for boot/crash diagnostics.

Captures whatever bytes arrive on a port for a fixed duration and writes
them to a file, plus prints a best-effort latin1 decode to the terminal.
Useful for catching ROM bootloader banners and Guru Meditation crash
dumps, which print *before* your app's Serial.begin() takes effect.

Baud rate gotcha: classic ESP32 (not S3/C3) ROM bootloader prints its
early boot log at 74880 baud, not the app's usual 115200. Read at the
wrong baud you get readable-looking garbage rather than silence, which
can be mistaken for no reset or no crash. If a device seems to go silent
right after a reset, try both bauds before concluding it is transmitting
nothing.

DTR/RTS gotcha: opening a port with pyserial asserts DTR/RTS by default,
and on boards with an auto-program circuit (DTR/RTS wired to EN/GPIO0),
that alone can hold the chip in reset or trigger repeated resets for as
long as a terminal stays attached, even with nothing being sent.
If a board seems to boot-loop only while something is monitoring it, and
boots fine standalone, this is almost certainly why. Pass --release-dtr-rts
to explicitly deassert both after opening (some circuits need this).

Usage:
    python3 raw_capture.py /dev/ttyACM0 [--baud 115200] [--seconds 15] \
        [--release-dtr-rts] [--out capture.bin]

Common bauds to try when the transmitting rate is unknown:
    74880   classic ESP32 ROM bootloader boot log
    115200  this project's standard app Serial.begin() rate
"""
import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="e.g. /dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=15.0)
    ap.add_argument("--release-dtr-rts", action="store_true",
                     help="Explicitly deassert DTR/RTS after opening")
    ap.add_argument("--out", default=None,
                     help="Path to write raw bytes to (default: none)")
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=1)
    if args.release_dtr_rts:
        ser.dtr = False
        ser.rts = False

    print(f"Capturing {args.port} @ {args.baud} baud for {args.seconds}s "
          f"(dtr={ser.dtr}, rts={ser.rts})...", file=sys.stderr)

    deadline = time.time() + args.seconds
    buf = b""
    while time.time() < deadline:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
    ser.close()

    print(f"Captured {len(buf)} bytes.", file=sys.stderr)
    if args.out:
        with open(args.out, "wb") as f:
            f.write(buf)
        print(f"Wrote raw bytes to {args.out}", file=sys.stderr)

    print(buf.decode("latin1", errors="replace"))


if __name__ == "__main__":
    main()
