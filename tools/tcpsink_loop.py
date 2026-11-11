"""TCP sink that survives more than one connection.

tcpsink.py accepts exactly one connection and exits. Used for repeated
throughput measurement it produces a spectacular lie: the first `c5iperf` reads
~24 Mbit/s, the second reads 338 kbit/s with ~4000 "window stall" events, because
nobody is draining the socket and the sender blocks on the receive window.

That number would have been written up as "Wi-Fi throughput is unstable". It is
not; it is a missing reader.

Usage: tcpsink_loop.py [port]
"""
import socket
import sys
import time


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 4444

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(4)
    print("listening on %d (loop)" % port, flush=True)

    total = 0
    while True:
        conn, addr = srv.accept()
        print("accepted from %s:%d" % addr, flush=True)
        start = time.time()
        n = 0
        try:
            while True:
                data = conn.recv(65536)
                if not data:
                    break
                n += len(data)
        except (ConnectionResetError, OSError):
            pass
        finally:
            conn.close()
        elapsed = time.time() - start
        total += n
        rate = (n * 8 / elapsed / 1000.0) if elapsed > 0 else 0.0
        print("connection done: %d bytes in %.1f s = %.0f kbit/s (running total %d)"
              % (n, elapsed, rate, total), flush=True)


if __name__ == "__main__":
    sys.exit(main())
