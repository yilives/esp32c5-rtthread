"""TCP source on the host: push a fixed burst to the board and report the rate.

The receive-direction counterpart of tools/tcpsink.py. Used by `c5rx`.

Usage: tcpsrc.py [port] [bytes] [chunk]
"""
import socket
import sys
import time


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 5555
    total = int(sys.argv[2]) if len(sys.argv) > 2 else 400000
    chunk = int(sys.argv[3]) if len(sys.argv) > 3 else 4096

    payload = bytes((i * 7) & 0xFF for i in range(chunk))

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(1)
    print("listening on %d, waiting for the board ..." % port, flush=True)

    conn, addr = srv.accept()
    print("accepted from %s:%d" % addr, flush=True)

    sent = 0
    t0 = time.time()
    conn.settimeout(20.0)
    try:
        while sent < total:
            n = conn.send(payload)
            if n <= 0:
                break
            sent += n
    except socket.timeout:
        print("send timed out after %d bytes" % sent, flush=True)
    except OSError as exc:
        print("send failed after %d bytes: %s" % (sent, exc), flush=True)

    elapsed = time.time() - t0
    print("host sent %d bytes in %.2f s = %.1f kbit/s"
          % (sent, elapsed, (sent * 8.0 / elapsed / 1000.0) if elapsed else 0),
          flush=True)

    time.sleep(1.0)
    conn.close()
    srv.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
