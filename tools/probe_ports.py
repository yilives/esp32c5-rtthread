"""Probe one serial port with hard timeouts.

Opened separately per invocation because a port that is already claimed by a
monitor blocks in open() and there is no timeout for that - the only way out is
a watchdog thread.
"""
import sys
import threading
import time

import serial


def probe(port: str, baud: int = 115200, listen: float = 3.0) -> None:
    result: dict[str, object] = {}

    def worker() -> None:
        try:
            sp = serial.Serial(port, baud, timeout=0.2, write_timeout=1.0)
            with sp:
                sp.reset_input_buffer()
                # Ctrl-C first: a shell prints its prompt, a bootloader says
                # nothing, and a Linux console echoes. All three are informative.
                sp.write(b"\r\n")
                sp.flush()
                deadline = time.time() + listen
                out = bytearray()
                while time.time() < deadline:
                    chunk = sp.read(512)
                    if chunk:
                        out += chunk
                result["data"] = bytes(out)
        except Exception as exc:                  # noqa: BLE001
            result["err"] = f"{type(exc).__name__}: {exc}"

    t = threading.Thread(target=worker, daemon=True)
    t.start()
    t.join(timeout=listen + 6.0)

    if t.is_alive():
        print(f"=== {port}: TIMEOUT (open blocked or read hung)")
        return

    if "err" in result:
        print(f"=== {port}: {result['err']}")
        return

    data = result.get("data", b"")
    print(f"=== {port}: {len(data)} bytes")
    if data:
        text = data.decode("utf-8", "replace")
        printable = "".join(
            c if 32 <= ord(c) < 127 or c in "\r\n\t" else "." for c in text
        )
        print(printable[:2000])


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: probe_ports.py <PORT> [baud]")
        sys.exit(2)
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
    probe(sys.argv[1], baud)
