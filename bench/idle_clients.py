#!/usr/bin/env python3
"""Opens many idle client connections and reports whether the server survives.

Uses:
  * Evidence for the select() limit. On Linux, fd_set holds sockets 0-1023 only.
    FD_SET on socket 1024 or higher is out of bounds; what happens then depends
    on the build (glibc's _FORTIFY_SOURCE check, AddressSanitizer, or nothing).
  * Idle background connections when comparing select() with epoll.

Each connection sends PING and waits for PONG before the next one opens, so the
exact connection that fails is known. If this script starts the server itself
(--server-bin), it also reports the server's highest socket number and exit
status, and saves the server's output, which is where sanitizer reports go.

Usage (on Linux):
  python3 bench/idle_clients.py --server-bin build/kosh-server --connections 1100
  python3 bench/idle_clients.py --port 6379 --connections 1100
"""

import argparse
import os
import resource
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import benchlib as bl


def parse_args():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    target = p.add_mutually_exclusive_group(required=True)
    target.add_argument("--server-bin", type=Path, help="start this Kosh binary")
    target.add_argument("--redis-server", help="start this real Redis binary (for comparison)")
    target.add_argument("--port", type=int, help="use a server already running on this port")
    p.add_argument("--connections", type=int, default=1100)
    p.add_argument("--hold", type=float, default=2.0,
                   help="seconds to keep all connections open before the final check")
    p.add_argument("--log", type=Path, help="save the server's output to this file")
    args = p.parse_args()
    if args.server_bin:
        # The server runs in a temporary directory, so a relative path would not resolve.
        args.server_bin = args.server_bin.resolve()
        if not args.server_bin.is_file():
            p.error(f"{args.server_bin} does not exist")
    return args


def highest_fd(pid):
    """Highest file descriptor the server process has open."""
    try:
        return max(int(fd) for fd in os.listdir(f"/proc/{pid}/fd"))
    except (OSError, ValueError):
        return None


def open_connections(port, count, srv=None):
    """Opens connections one by one, each answering PING before the next opens.
    Returns (sockets, how many worked, error or None, highest server fd)."""
    sockets, max_fd = [], None
    for i in range(1, count + 1):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=5)
            sockets.append(s)
            s.sendall(b"*1\r\n$4\r\nPING\r\n")
            reply = b""
            while not reply.endswith(b"\r\n"):
                chunk = s.recv(64)
                if not chunk:
                    raise ConnectionError("connection closed by the server")
                reply += chunk
            if reply != b"+PONG\r\n":
                raise ConnectionError(f"unexpected reply {reply!r}")
        except OSError as e:
            return sockets, i - 1, f"connection {i} failed: {e or type(e).__name__}", max_fd
        if srv is not None:
            max_fd = highest_fd(srv.proc.pid) or max_fd
    return sockets, count, None, max_fd


def main():
    args = parse_args()
    # Each connection is a file descriptor in this process, and the server inherits
    # the same limit; raise the soft limit as far as the hard limit allows.
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    want = args.connections + 64
    if soft < want:
        resource.setrlimit(resource.RLIMIT_NOFILE, (min(want, hard), hard))

    with tempfile.TemporaryDirectory(prefix="kosh-idle-") as workdir:
        srv = None
        port = args.port
        if args.port is None:
            port = bl.free_port()
            if args.server_bin:
                argv = bl.kosh_argv(args.server_bin, port, workdir, False, 10_000)
            else:
                argv = bl.redis_argv(args.redis_server, port, workdir, False) + \
                       ["--maxclients", str(args.connections + 100)]
            srv = bl.Server(argv, workdir, port)
            srv.wait_ready()

        start = time.monotonic()
        sockets, opened, error, max_fd = open_connections(port, args.connections, srv)
        if error is None:
            time.sleep(args.hold)
            try:
                bl.ping(port)
            except OSError as e:
                error = f"new connection after holding {opened} open failed: {e}"

        if srv is not None:
            if error is not None:
                # A crashing server may still be writing its sanitizer report;
                # stopping it now would cut the report short.
                try:
                    srv.proc.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    pass
            status = bl.describe_exit(srv.proc.poll())
            srv.stop()
        for s in sockets:
            s.close()

        print(f"Opened {opened} of {args.connections} connections "
              f"in {time.monotonic() - start:.1f} s")
        if max_fd is not None:
            print(f"Server's highest file descriptor while all were working: {max_fd}")
        print(f"Result: {error or 'all connections answered PING'}")
        if srv is not None:
            print(f"Server: {status}")
            if args.log:
                shutil.copyfile(srv.log_path, args.log)
                print(f"Server output saved to {args.log}")
            else:
                # Kosh's stdout is block-buffered when it goes to a file, so a
                # killed server can lose it; crash reports go to unbuffered stderr.
                print("Last lines of server output:\n" + (srv.log_tail(15) or "(no output)"))
    sys.exit(0 if error is None else 1)


if __name__ == "__main__":
    main()
