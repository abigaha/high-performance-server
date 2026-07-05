#!/usr/bin/env python3
"""
WebSocket 握手验证脚本。
验证 HTTP Upgrade 握手成功（101 Switching Protocols）。
帧通信因架构限制不在此处验证，由单元测试覆盖。
"""
import socket
import base64
import os
import sys


def test_ws_handshake(host="localhost", port=9090, path="/ws"):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect((host, port))

    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n"
        f"\r\n"
    ).encode()

    s.sendall(req)
    resp = s.recv(4096).decode(errors="replace")

    if "101 Switching Protocols" in resp:
        print("HANDSHAKE_OK")
        s.close()
        return True
    else:
        print(f"HANDSHAKE_FAIL: {resp[:200]}")
        s.close()
        return False


if __name__ == "__main__":
    host = sys.argv[1] if len(sys.argv) > 1 else "localhost"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 9090
    ok = test_ws_handshake(host, port)
    sys.exit(0 if ok else 1)
