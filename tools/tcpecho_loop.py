"""Echo server that survives more than one connection.

tcpecho.py accepts exactly one connection and exits - fine for a single
measurement, useless for a repeatability test. Written after three consecutive
`c5tcp` runs all reported errno=104 (ECONNRESET): the first connection was
answered and the server had already gone, so runs 2 and 3 got a host RST. That
looked like a device fault for two rounds.

Usage: tcpecho_loop.py [port] [reply]
"""
import socket
import sys


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 3333
    reply = (sys.argv[2] if len(sys.argv) > 2 else "pong-from-pc").encode()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(4)
    print("listening on %d (loop)" % port, flush=True)

    while True:
        conn, addr = srv.accept()
        print("accepted from %s:%d" % addr, flush=True)
        conn.settimeout(15.0)
        try:
            while True:
                data = conn.recv(512)
                if not data:
                    break
                print("received %d byte(s): %r" % (len(data), data), flush=True)
                conn.sendall(reply)
                print("replied: %r" % reply, flush=True)
        except (socket.timeout, ConnectionResetError):
            pass
        finally:
            conn.close()


if __name__ == "__main__":
    sys.exit(main())
