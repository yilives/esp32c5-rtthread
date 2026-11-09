"""Reset the ESP32-C5 with esptool's own line sequence, then read the console.

Why this exists
---------------
`tools/reset.py` drives DTR low to assert reset, which is the polarity of the
S31 board's inverting buffer. On this C5 board the reset line is RTS and IO0 is
DTR, the same way esptool drives them - so that script asserts the wrong line and
the board never reboots, which looks exactly like a dead image.

`--mode run`   : reset into the application (IO0 released).
`--mode boot0` : hold IO0 low while resetting, i.e. the ROM download mode. If
                 this prints "waiting for download" the console wiring is fine and
                 the problem is in the image; if it prints nothing either, the
                 console is not on this port.

Usage: c5reset.py <PORT> [--mode run|boot0] [--seconds N]
"""
import sys
import time

import serial


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2

    port = args[0]
    mode = "run"
    seconds = 8.0
    for i, a in enumerate(args):
        if a == "--mode":
            mode = args[i + 1]
        elif a == "--seconds":
            seconds = float(args[i + 1])

    with serial.Serial(port, 115200, timeout=0.05, write_timeout=2.0) as sp:
        # esptool's default_reset: DTR -> IO0, RTS -> EN.
        sp.dtr = False
        sp.rts = True          # EN low: chip held in reset
        time.sleep(0.25)

        if mode == "boot0":
            sp.dtr = True      # IO0 low while EN rises -> download mode
            time.sleep(0.1)
            sp.rts = False
        else:
            sp.dtr = False     # IO0 released -> normal boot
            time.sleep(0.1)
            sp.rts = False

        sp.reset_input_buffer()

        out = bytearray()
        deadline = time.time() + seconds
        while time.time() < deadline:
            chunk = sp.read(4096)
            if chunk:
                out += chunk

    text = out.decode("utf-8", "replace").replace("\r", "")
    print("--- %d bytes (%s) ---" % (len(out), mode))
    sys.stdout.write(text)
    print()
    return 0 if out else 1


if __name__ == "__main__":
    sys.exit(main())
