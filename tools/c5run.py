"""Run one msh command on the C5 and collect output for N seconds.

c5cmd.py waits 12 s per command, which is enough for the bring-up checks but not
for a passive Wi-Fi scan (~12 s on this board under stock FreeRTOS) or a RADIO
init. This variant does not look for the prompt; it just types the command after
the shell is up and streams for the requested window.

Usage: c5run.py <PORT> <command> [seconds]
"""
import sys
import time

import serial

PROMPT = b"msh >"


def main() -> int:
    port = sys.argv[1]
    command = sys.argv[2]
    seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 30.0

    with serial.Serial(port, 115200, timeout=0.05, write_timeout=2.0) as sp:
        # esptool reset polarity: DTR -> IO0, RTS -> EN.
        sp.dtr = True
        sp.rts = True
        time.sleep(0.2)
        sp.dtr = False
        time.sleep(0.1)
        sp.rts = False
        sp.reset_input_buffer()

        # Wait for the prompt, then type. The shell prints it in response to input,
        # so nudge once after the boot window.
        deadline = time.time() + 12
        seen = bytearray()
        while time.time() < deadline:
            chunk = sp.read(4096)
            if chunk:
                seen += chunk
                if PROMPT in seen:
                    break
        if PROMPT not in seen:
            sp.write(b"\r\n")
            sp.flush()

        sp.write((command + "\r\n").encode())
        sp.flush()

        out = bytearray()
        deadline = time.time() + seconds
        while time.time() < deadline:
            chunk = sp.read(4096)
            if chunk:
                out += chunk

    text = out.decode("utf-8", "replace").replace("\r", "")
    print("--- %d bytes ---" % len(out))
    sys.stdout.write(text)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
