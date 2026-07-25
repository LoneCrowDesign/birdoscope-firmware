#!/usr/bin/env python3
"""List connected serial devices with vendor/product info where available.

Usage: python3 list_ports.py
"""
import serial.tools.list_ports


def main():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return
    for p in ports:
        print(f"{p.device}  vid={p.vid:#06x} pid={p.pid:#06x}  {p.description}"
              if p.vid is not None else f"{p.device}  {p.description}")


if __name__ == "__main__":
    main()
