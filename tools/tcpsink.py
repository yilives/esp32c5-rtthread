"""TCP sink on the host: count bytes from the board and report the rate.

Written because the first attempt used a PowerShell sink that polled
`DataAvailable` every 2 ms, and the board's `send()` then blocked for tens of
seconds with nothing printed - the board looked hung while both sides were
technically working. A sink that drains in a tight loop separates "the link is
slow" from "the sender is blocked".

Usage: tcpsink.py [port] [seconds]
"""
import socket
import sys
import time


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 4444
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(1)
    print("listening on %d, waiting for the board ..." % port, flush=True)

    conn, addr = srv.accept()
    print("accepted from %s:%d" % addr, flush=True)

    total = 0
    t0 = time.time()
    marks = []
    last_mark = t0

    conn.settimeout(1.0)
    while time.time() - t0 < seconds:
        try:
            chunk = conn.recv(16384)
        except socket.timeout:
            chunk = b""
            if time.time() - t0 > 3 and total == 0:
                break
        if chunk == b"" and time.time() - last_mark > 2.0:
            # keep waiting until the deadline; the board may be between bursts
            pass
        if chunk:
            total += len(chunk)
        now = time.time()
        if now - last_mark >= 1.0:
            marks.append(total)
            last_mark = now

    elapsed = time.time() - t0
    conn.close()
    srv.close()

    print("per-second cumulative bytes: %s" % marks[:12])
    print("sink received %d bytes in %.1f s = %.1f kbit/s"
          % (total, elapsed, (total * 8.0 / elapsed / 1000.0) if elapsed else 0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
