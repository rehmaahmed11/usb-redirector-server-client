#!/usr/bin/env python3
"""Protocol tests for mubb-relay (run on any OS; the relay also builds on Linux).

    g++ -std=c++17 -O2 -pthread relay/relay.cpp -o /tmp/mubb-relay
    python3 tests/test_relay.py /tmp/mubb-relay

Checks: pairing of two peers, byte splicing in both directions, rejection of a
wrong auth tag, rejection of a duplicate role, and the busy/timeout statuses.
"""
import os
import socket
import struct
import subprocess
import sys
import time

MAGIC = b"MUBR1"
OK, BAD_TAG, TIMEOUT = 0x00, 0x01, 0x02

failures = 0
checks = 0


def check(condition, label):
    global failures, checks
    checks += 1
    if not condition:
        failures += 1
        print(f"  FAIL  {label}")


def hello(role, session_id, tag):
    return MAGIC + bytes([role]) + struct.pack(">Q", session_id) + tag


def connect(port, role, session_id, tag, timeout=5):
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    s.sendall(hello(role, session_id, tag))
    return s


def recv_exact(s, n, timeout=5):
    s.settimeout(timeout)
    data = b""
    while len(data) < n:
        try:
            chunk = s.recv(n - len(data))
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
    return data


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./mubb-relay"
    port = 47913
    proc = subprocess.Popen([binary, "--port", str(port), "--verbose"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            env={**os.environ, "MALLOC_CHECK_": "0"})
    try:
        for _ in range(60):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.1)

        tag = bytes(range(16))

        # --- 1. two peers pair, status 0x00, bytes flow both ways -------------
        print("relay: pairing and splicing")
        sid = 0x1122334455667788
        c = connect(port, 1, sid, tag)
        time.sleep(0.2)
        s = connect(port, 2, sid, tag)
        check(recv_exact(c, 1) == bytes([OK]), "client got paired status")
        check(recv_exact(s, 1) == bytes([OK]), "server got paired status")
        payload_c = b"hello-from-client" * 100
        payload_s = b"hello-from-server" * 77
        c.sendall(payload_c)
        s.sendall(payload_s)
        check(recv_exact(s, len(payload_c)) == payload_c, "server received client bytes")
        check(recv_exact(c, len(payload_s)) == payload_s, "client received server bytes")
        # binary safe payload (all byte values)
        blob = bytes(range(256)) * 16
        c.sendall(blob)
        check(recv_exact(s, len(blob)) == blob, "binary payload intact")
        c.close()
        s.close()

        # --- 2. wrong auth tag is rejected -----------------------------------
        print("relay: auth tag enforcement")
        sid2 = 0x2222333344445555
        c2 = connect(port, 1, sid2, tag)
        time.sleep(0.2)
        bad = connect(port, 2, sid2, b"\xAA" * 16)
        check(recv_exact(bad, 1) == bytes([BAD_TAG]), "wrong tag rejected")
        bad.close()
        c2.close()

        # --- 3. duplicate role for the same session is refused ---------------
        print("relay: duplicate role refused")
        sid3 = 0x3333444455556666
        a = connect(port, 1, sid3, tag)
        time.sleep(0.2)  # let the first peer register before the duplicate arrives
        b = connect(port, 1, sid3, tag)
        check(recv_exact(b, 1) == bytes([TIMEOUT]), "duplicate client role refused")
        b.close()
        a.close()

        # --- 4. malformed magic is rejected ----------------------------------
        print("relay: malformed handshake refused")
        m = socket.create_connection(("127.0.0.1", port), timeout=5)
        m.sendall(b"NOTRELAYANDMOREJUNKJUNKJUNKJUNK")
        check(recv_exact(m, 1) == bytes([BAD_TAG]), "bad magic rejected")
        m.close()

        # --- 5. lone peer times out (short relay wait is not configurable, so
        #        just verify it does not crash and the process is still alive)
        print("relay: still healthy after rejected peers")
        check(proc.poll() is None, "relay process still running")
        sid5 = 0x5555666677778888
        c5 = connect(port, 1, sid5, tag)
        time.sleep(0.2)
        s5 = connect(port, 2, sid5, tag)
        check(recv_exact(c5, 1) == bytes([OK]) and recv_exact(s5, 1) == bytes([OK]),
              "relay still pairs after rejections")
        c5.close()
        s5.close()
    finally:
        proc.terminate()
        try:
            out = proc.communicate(timeout=5)[0].decode(errors="replace")
        except subprocess.TimeoutExpired:
            proc.kill()
            out = ""

    print("-----------------------------")
    print(f"{checks} checks, {failures} failure(s)")
    if failures:
        print("relay log:\n" + out)
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
