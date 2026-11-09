"""One-shot TCP echo peer for `c5tcp`: accept, read, reply, exit.

Written because the same test in PowerShell (TcpListener inside Start-Job) has no
accept timeout: when the board failed to connect, `Receive-Job -Wait` blocked
forever and took the whole harness with it. This has a deadline on every step, so
a failure is reported instead of hanging.

Usage: tcpecho.py [port] [reply]
"""
import socket
import sys


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 3333
    reply = (sys.argv[2] if len(sys.argv) > 2 else "pong-from-pc").encode()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(1)
    srv.settimeout(45.0)
    print("listening on %d" % port, flush=True)

    try:
        conn, addr = srv.accept()
    except socket.timeout:
        print("RESULT: no connection within 45 s", flush=True)
        srv.close()
        return 1

    print("accepted from %s:%d" % addr, flush=True)
    conn.settimeout(15.0)

    try:
        data = conn.recv(512)
    except socket.timeout:
        print("RESULT: connected but sent nothing", flush=True)
        conn.close()
        srv.close()
        return 1

    print("received %d byte(s): %s" % (len(data), data.decode("latin-1")), flush=True)
    conn.sendall(reply)
    print("replied: %s" % reply.decode("latin-1"), flush=True)

    conn.close()
    srv.close()
    print("RESULT: PASS", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
