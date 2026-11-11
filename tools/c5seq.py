"""Reset the C5 once, then run several msh commands in the same session.

c5cmd.py and c5run.py both reset first, which is fine for one command but wrong
for anything stateful: `c5tcp` needs the association that `c5all` established, and
a reset in between wipes it. Measured that way: `c5tcp` on a freshly reset board
trips lwIP's own `tcpip_send_msg_wait_sem` assert ("Invalid mbox") because the
tcpip thread does not exist yet.

Usage: c5seq.py <PORT> "cmd1" "cmd2" ...
       c5seq.py <PORT> --wait 40 "cmd1" "cmd2"
"""
import sys
import time

import serial

PROMPT = b"msh >"

# Longest silence a command may produce before it is assumed finished.
# Measured: a 5 GHz scan prints nothing for ~10 s, association for ~15 s.
IDLE_LIMIT = 25.0


def main() -> int:
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        return 2

    port = args[0]
    rest = args[1:]
    wait = 8.0
    if rest and rest[0] == "--wait":
        wait = float(rest[1])
        rest = rest[2:]

    with serial.Serial(port, 115200, timeout=0.05, write_timeout=2.0) as sp:
        # esptool polarity: DTR -> IO0, RTS -> EN.
        sp.dtr = True
        sp.rts = True
        time.sleep(0.2)
        sp.dtr = False
        time.sleep(0.1)
        sp.rts = False
        sp.reset_input_buffer()

        # Boot window: wait for the shell.
        boot = bytearray()
        deadline = time.time() + 12
        while time.time() < deadline:
            chunk = sp.read(4096)
            if chunk:
                boot += chunk
                if PROMPT in boot:
                    break
        if PROMPT not in boot:
            sp.write(b"\r\n")
            sp.flush()
        print("=== boot ok (%d bytes) ===" % len(boot))

        for cmd in rest:
            sp.write((cmd + "\r\n").encode())
            sp.flush()

            # RETURN WHEN THE PROMPT COMES BACK, not after a fixed delay.
            #
            # `--wait` used to be how long to read, in full, for every command:
            #
            #     deadline = time.time() + wait
            #     while time.time() < deadline: out += sp.read(4096)
            #
            # So `--wait 700` with six commands took 4200 seconds regardless of
            # what the commands did, and a four-join sequence looked hung for
            # 20+ minutes with no output. It was not hung; it was sleeping.
            #
            # `--wait` is now an UPPER BOUND: reading stops as soon as the prompt
            # reappears, which is the only signal that the command finished.
            out = bytearray()
            deadline = time.time() + wait
            idle_since = time.time()
            while time.time() < deadline:
                chunk = sp.read(4096)
                if chunk:
                    out += chunk
                    idle_since = time.time()
                    # The prompt is printed after the command returns. Require it
                    # to arrive after some output, so a stale prompt from the
                    # previous command cannot end this one immediately.
                    # The prompt must END what has arrived, not merely appear in
                    # it. `PROMPT in out` matched the prompt left over from the
                    # PREVIOUS command as soon as a few bytes of WiFi log had
                    # arrived, so `c5join5` was cut off mid-connection - which
                    # looked exactly like the board rebooting mid-sequence and
                    # produced a false "join x4 restarts" reading.
                    if out.rstrip().endswith(PROMPT):
                        break
                elif (time.time() - idle_since) > IDLE_LIMIT and len(out) > 0:
                    # Fallback for a swallowed prompt. It has to be LONGER than
                    # the quietest legitimate pause: a Wi-Fi scan prints nothing
                    # for ~10 s, and at 3 s this cut `c5all` off after `set_ps`,
                    # which reads exactly like a hang in the driver.
                    break

            text = out.decode("utf-8", "replace").replace("\r", "")
            print("\n=== %s ===" % cmd)
            sys.stdout.write(text)

    return 0


if __name__ == "__main__":
    sys.exit(main())
