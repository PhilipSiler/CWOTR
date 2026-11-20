#!/usr/bin/env python3
"""Hambot's connection: dials a referee's WebSocket and relays the WotR Bot Protocol between it and a
program speaking the protocol on stdin/stdout (one JSON object per line), here hambot. Standard library only.

    ws_stdio.py --connect wss://HOST/referee --token-file FILE -- COMMAND ARGS...

The token (a sandbox key, an online bot's key or a tournament seat token) is read from a file, so it is on no command
line, and sent as "Authorization: Bearer"; WOTR_TOKEN in the environment works too. Exits when the referee closes the
connection or the program ends."""
import argparse, base64, os, socket, ssl, struct, subprocess, sys, threading
from urllib.parse import urlparse


class WebSocket:
    def __init__(self, url, token=None):
        parts = urlparse(url)
        secure = parts.scheme == "wss"
        port = parts.port or (443 if secure else 80)
        raw = socket.create_connection((parts.hostname, port))
        raw.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock = ssl.create_default_context().wrap_socket(raw, server_hostname=parts.hostname) if secure else raw
        key = base64.b64encode(os.urandom(16)).decode()
        path = (parts.path or "/") + ("?" + parts.query if parts.query else "")
        host = parts.hostname + ("" if parts.port is None else ":%d" % parts.port)
        request = ("GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n%s\r\n") % (
            path, host, key, "Authorization: Bearer %s\r\n" % token if token else "")
        self.sock.sendall(request.encode())
        self.buffer = b""
        self.lock = threading.Lock()
        header = self._read_until(b"\r\n\r\n").decode("latin-1")
        if " 101 " not in header.split("\r\n")[0]:
            raise RuntimeError("the referee refused the connection: " + header.split("\r\n")[0])

    def _read(self, n):
        while len(self.buffer) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("connection closed")
            self.buffer += chunk
        data, self.buffer = self.buffer[:n], self.buffer[n:]
        return data

    def _read_until(self, marker):
        while marker not in self.buffer:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("connection closed")
            self.buffer += chunk
        index = self.buffer.index(marker) + len(marker)
        data, self.buffer = self.buffer[:index], self.buffer[index:]
        return data

    def _frame(self, opcode, payload):
        header = bytes([0x80 | opcode])
        n = len(payload)
        if n < 126:
            header += bytes([0x80 | n])
        elif n < 65536:
            header += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            header += bytes([0x80 | 127]) + struct.pack(">Q", n)
        mask = os.urandom(4)  # A client masks every frame.
        with self.lock:
            self.sock.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def send(self, text):
        self._frame(0x1, text.encode("utf-8"))

    def receive(self):
        """The next text message, or None once the referee closed the connection."""
        message = b""
        while True:
            b0, b1 = self._read(2)
            opcode, n = b0 & 0x0F, b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._read(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._read(8))[0]
            mask = self._read(4) if b1 & 0x80 else None
            payload = self._read(n)
            if mask:
                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
            if opcode == 0x8:
                return None
            if opcode == 0x9:
                self._frame(0xA, payload)
                continue
            if opcode in (0x0, 0x1):
                message += payload
                if b0 & 0x80:
                    return message.decode("utf-8")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--connect", required=True)
    ap.add_argument("--token-file")
    ap.add_argument("command", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    command = a.command[1:] if a.command[:1] == ["--"] else a.command
    if not command:
        ap.error("no program to run")
    token = open(a.token_file).read().strip() if a.token_file else os.environ.get("WOTR_TOKEN")
    ws = WebSocket(a.connect, token)
    child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)

    def to_referee():
        for line in child.stdout:
            line = line.decode("utf-8").strip()
            if line:
                try:
                    ws.send(line)
                except OSError:
                    return

    threading.Thread(target=to_referee, daemon=True).start()
    try:
        while True:
            msg = ws.receive()
            if msg is None:
                break
            child.stdin.write(msg.encode("utf-8") + b"\n")
            child.stdin.flush()
    except (EOFError, OSError, BrokenPipeError) as e:
        print("ws_stdio: %s" % e, file=sys.stderr)
    try:
        child.stdin.close()
        child.wait(timeout=5)
    except Exception:
        child.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
