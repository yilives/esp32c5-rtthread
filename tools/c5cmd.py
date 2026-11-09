"""Reset the ESP32-C5 into the app, then type msh commands and capture replies.

The shell prints its prompt only in response to input, and the reset lines are
esptool-polarity (DTR -> IO0, RTS -> EN), so both have to be done explicitly -
see c5reset.py for why the S31 helper drives the wrong line here.

Usage: c5cmd.py <PORT> <command> [command...]
"""
import sys
import time

import serial

PROMPT = b"msh >"


def read_some(sp: serial.Serial, seconds: float) -> bytes:
    out = bytearray()
    deadline = time.time() + seconds
    while time.time() < deadline:
        chunk = sp.read(4096)
        if chunk:
            out += chunk
    return bytes(out)


def wait_for(sp: serial.Serial, needle: bytes, timeout: float) -> bytes:
    out = bytearray()
    deadline = time.time() + timeout
    tail = b""
    while time.time() < deadline:
        chunk = sp.read(4096)
        if chunk:
            out += chunk
            tail = (tail + chunk)[-len(needle):]
            if needle in out or tail == needle:
                break
    return bytes(out)


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2

    port = sys.argv[1]
    commands = sys.argv[2:]

    with serial.Serial(port, 115200, timeout=0.05, write_timeout=2.0) as sp:
        # esptool default_reset, then release into the application.
        sp.dtr = True
        sp.rts = True
        time.sleep(0.2)
        sp.dtr = False
        time.sleep(0.1)
        sp.rts = False
        sp.reset_input_buffer()

        boot = wait_for(sp, PROMPT, 8.0)
        text = boot.decode("utf-8", "replace").replace("\r", "")
        print("=== boot ===")
        sys.stdout.write(text[-1200:])
        if PROMPT not in boot:
            print("\n!! no prompt after reset, stopping")
            return 1

        for cmd in commands:
            sp.write((cmd + "\r\n").encode())
            sp.flush()
            reply = wait_for(sp, PROMPT, 12.0)
            print("\n=== %s ===" % cmd)
            body = reply.decode("utf-8", "replace").replace("\r", "")
            # Drop the echoed command line so the output reads as a reply.
            lines = body.split("\n")
            if lines and lines[0].strip().endswith(cmd):
                lines = lines[1:]
            sys.stdout.write("\n".join(lines))

    return 0


if __name__ == "__main__":
    sys.exit(main())
