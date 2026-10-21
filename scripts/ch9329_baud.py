#!/usr/bin/env python3
"""Read or configure a CH9329 baud rate through its binary serial protocol."""

import argparse
import sys
import time

BAUD_RATES = (1200, 2400, 4800, 9600, 14400, 19200, 38400, 57600, 115200)


def frame(address, command, data=b""):
    packet = bytes((0x57, 0xAB, address, command, len(data))) + data
    return packet + bytes((sum(packet) & 0xFF,))


def read_exact(port, size):
    data = bytearray()
    deadline = time.monotonic() + 2.0
    while len(data) < size:
        if time.monotonic() >= deadline:
            raise RuntimeError("Timed out waiting for CH9329 response")
        part = port.read(size - len(data))
        data.extend(part)
    return bytes(data)


def exchange(port, address, command, data=b""):
    packet = frame(address, command, data)
    if port.write(packet) != len(packet):
        raise RuntimeError("Incomplete serial write")
    port.flush()
    header = read_exact(port, 5)
    if header[:2] != b"\x57\xab" or header[2] != address or header[4] > 64:
        raise RuntimeError(f"Invalid response header: {header.hex(' ')}")
    tail = read_exact(port, header[4] + 1)
    if (sum(header + tail[:-1]) & 0xFF) != tail[-1]:
        raise RuntimeError("Invalid response checksum")
    if header[3] != (command | 0x80):
        raise RuntimeError(
            f"Command 0x{command:02x} returned 0x{header[3]:02x}: "
            f"{tail[:-1].hex(' ')}"
        )
    return tail[:-1]


def read_config(port, address):
    config = bytearray(exchange(port, address, 0x08))
    if len(config) != 50:
        raise RuntimeError(f"Expected 50 configuration bytes, got {len(config)}")
    return config


def configure(port, address, new_baud):
    config = read_config(port, address)
    current = int.from_bytes(config[3:7], "big")
    print(f"Configured baud: {current}")
    if new_baud is None:
        return
    if current == new_baud:
        print("Already configured; no write")
        return

    # GET may return pin-selected modes (0x80+); SET accepts software modes only.
    if config[0] not in (0, 1, 2, 3, 0x80, 0x81, 0x82, 0x83):
        raise RuntimeError(f"Unknown work mode: 0x{config[0]:02x}")
    if config[1] not in (0, 1, 2, 0x80, 0x81, 0x82):
        raise RuntimeError(f"Unknown serial mode: 0x{config[1]:02x}")
    if config[1] & 0x7F:
        raise RuntimeError("Use protocol serial mode (0) before changing baud")
    config[0] &= 0x7F
    config[1] &= 0x7F
    config[3:7] = new_baud.to_bytes(4, "big")
    if exchange(port, address, 0x09, config) != b"\x00":
        raise RuntimeError("CH9329 rejected configuration")
    print(f"Saved baud: {new_baud}")
    print(f"Power-cycle the CH9329, then read again with --baud {new_baud} "
          "and without --set-baud to verify the new setting")


def address_value(text):
    value = int(text, 0)
    if not 0 <= value < 0xFF:
        raise argparse.ArgumentTypeError("address must be 0..254 (not broadcast)")
    return value


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="serial device, e.g. /dev/ttyUSB0 or /dev/ttyCH341USB0")
    parser.add_argument("--baud", type=int, choices=BAUD_RATES, default=9600,
                        help="current serial link rate (default: 9600)")
    parser.add_argument("--set-baud", type=int, choices=BAUD_RATES,
                        help="save this rate; omit to read only")
    parser.add_argument("--address", type=address_value, default=0,
                        help="CH9329 address, decimal or 0x-prefixed (default: 0)")
    return parser.parse_args(argv)


def main():
    args = arguments()
    try:
        import serial
    except ImportError:
        print("Install pyserial: sudo apt install python3-serial", file=sys.stderr)
        return 1
    try:
        with serial.Serial(args.port, args.baud, timeout=0.2, write_timeout=2,
                           exclusive=True) as port:
            port.reset_input_buffer()
            configure(port, args.address, args.set_baud)
    except (OSError, serial.SerialException, RuntimeError, ValueError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
