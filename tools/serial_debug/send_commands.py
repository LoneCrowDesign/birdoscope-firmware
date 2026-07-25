#!/usr/bin/env python3
"""Send a sequence of word-based serial commands to a running birdoscope
board and print the responses.

Commands are word-based and newline-terminated. Every board's
checkSerialCommands() dispatches them through a shared core tokenizer, as a
verb plus an optional argument:
    status                          board-specific status line (printStatus())
    inject                          inject a synthetic test detection
    log                             dump the raw SD log (SD boards only)
    dump                            dump current in-memory session (core)
    prev                            dump /prev_session.json (core)
    nav <up|down|select|back|mark>  inject a nav event (screen/menu machine)
    chirp / jingle                  replay a buzzer tone (buzzer boards)
    help                            list commands (also '?')

Usage (quote any command that contains a space):
    python3 send_commands.py /dev/ttyACM0 status dump prev
    python3 send_commands.py /dev/ttyACM0 "nav down" "nav down" "nav select"
    python3 send_commands.py /dev/ttyACM0 inject inject dump --delay 1.5

Each argument after the port is sent as one full command line (terminated
with '\\n'), with a pause between sends to let the board respond.
"""
import argparse
import time

import serial


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="e.g. /dev/ttyACM0")
    ap.add_argument("commands", nargs="+",
                     help="word commands to send in order, e.g. status dump 'nav up'")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--delay", type=float, default=1.0,
                     help="seconds to wait after each command before reading")
    ap.add_argument("--boot-settle", type=float, default=2.0,
                     help="seconds to wait after opening the port before sending anything")
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=2)
    time.sleep(args.boot_settle)
    ser.reset_input_buffer()

    for cmd in args.commands:
        ser.write((cmd + "\n").encode())
        time.sleep(args.delay)
        resp = ser.read(4000)
        print(f"--- {cmd!r} ---")
        print(resp.decode(errors="replace"))

    ser.close()


if __name__ == "__main__":
    main()
