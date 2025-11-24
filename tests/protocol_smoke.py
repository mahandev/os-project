#!/usr/bin/env python3
"""End-to-end smoke test for the messaging server.

Usage:
    python3 tests/protocol_smoke.py

The script expects compiled binaries in ./bin via `make` and will:
1. Launch the server on an ephemeral port.
2. Connect two simulated clients (alice, bob).
3. Exchange a message and validate delivery + history + deletion + user list.
"""
from __future__ import annotations

import contextlib
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

PORT = 6200
TIMEOUT = 3
ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "bin"
SERVER_BIN = BIN / "server"


def recv_line(sock: socket.socket) -> str:
    data = bytearray()
    deadline = time.time() + TIMEOUT
    while time.time() < deadline:
        try:
            chunk = sock.recv(1)
        except socket.timeout as exc:  # pragma: no cover - best effort
            raise TimeoutError("Timed out waiting for server data") from exc
        if not chunk:
            raise RuntimeError("Connection closed unexpectedly")
        if chunk == b"\r":
            continue
        if chunk == b"\n":
            return data.decode()
        data.extend(chunk)
    raise TimeoutError("Timed out waiting for newline")


def send_line(sock: socket.socket, text: str) -> None:
    sock.sendall((text + "\n").encode())


def connect_user(username: str) -> socket.socket:
    sock = socket.create_connection(("127.0.0.1", PORT), timeout=TIMEOUT)
    recv_line(sock)  # welcome banner
    send_line(sock, f"AUTH {username}")
    response = recv_line(sock)
    if not response.startswith("OK"):
        raise RuntimeError(f"Auth failed for {username}: {response}")
    sock.settimeout(TIMEOUT)
    return sock


def main() -> int:
    if not SERVER_BIN.exists():
        print("Build the project first: make", file=sys.stderr)
        return 1

    db_fd, db_path = tempfile.mkstemp(prefix="chat-smoke-", suffix=".db")
    os.close(db_fd)
    server = subprocess.Popen(
        [str(SERVER_BIN), str(PORT), db_path],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    time.sleep(0.2)
    if server.poll() is not None:
        stderr = server.stderr.read()
        print(f"Server failed to start: {stderr}", file=sys.stderr)
        return 1
    deadline = time.time() + 5
    while time.time() < deadline:
        with contextlib.suppress(OSError):
            probe = socket.create_connection(("127.0.0.1", PORT), timeout=0.2)
            probe.close()
            break
        time.sleep(0.1)
    else:
        print("Timed out waiting for server port", file=sys.stderr)
        server.terminate()
        return 1
    alice = None
    bob = None
    try:
        alice = connect_user("alice")
        bob = connect_user("bob")

        send_line(alice, "SEND bob hello-bob")
        ok = recv_line(alice)
        assert ok.startswith("OK"), ok
        msg = recv_line(bob)
        assert msg.startswith("MESSAGE alice"), msg

        send_line(bob, "GET alice")
        history_line = recv_line(bob)
        assert history_line.startswith("HISTORY"), history_line

        # finish history stream
        while True:
            line = recv_line(bob)
            if line == "OK History end" or line.startswith("INFO "):
                break

        send_line(bob, "DELETE alice")
        assert recv_line(bob).startswith("OK"), "delete failed"

        send_line(alice, "USERS")
        assert recv_line(alice) == "USERS_BEGIN"
        users = []
        while True:
            entry = recv_line(alice)
            if entry == "USERS_END":
                break
            if entry.startswith("USER "):
                users.append(entry.split(" ", 1)[1])
        assert set(users) >= {"alice", "bob"}

        send_line(alice, "QUIT")
        send_line(bob, "QUIT")
    finally:
        for sock in (alice, bob):
            if sock:
                try:
                    sock.close()
                except OSError:
                    pass
        server.send_signal(signal.SIGINT)
        try:
            server.wait(timeout=2)
        except subprocess.TimeoutExpired:
            server.kill()
        if os.path.exists(db_path):
            os.remove(db_path)
    print("Smoke test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
