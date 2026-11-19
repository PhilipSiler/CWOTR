#!/usr/bin/env python3
"""Connects CWOTR, as a bot, to a WotR Bot Protocol referee over a WebSocket (SPEC.md, sections 2, 12 and 14).

    python3 cwotr_connect.py --connect wss://HOST/referee            (the token is read from the environment: CWOTR_TOKEN)
    python3 cwotr_connect.py --connect "ws://localhost:8765/referee" --token SEAT-OR-BOT-TOKEN [--shadow hunter|conquest]

The referee holds the rules; CWOTR only ever answers with one of the options it lists. This script is the wire: it
dials the referee, starts CWOTR's player (cwotr-referee bot <player>) and passes each message of the referee to it and
each answer back. It ends when the referee says quit or closes the connection. The token is sent in the address
(?token=...) and is never printed. Standard library only.
"""
import argparse, base64, json, os, socket, ssl, struct, subprocess, sys
from urllib.parse import urlparse

HERE = os.path.dirname(os.path.abspath(__file__))

def log(*a):
    print(*a, file=sys.stderr); sys.stderr.flush()

class WebSocket:
    """A minimal WebSocket client (RFC 6455): text messages, ping/pong, close."""
    def __init__(self, url):
        p = urlparse(url); secure = p.scheme == "wss"; port = p.port or (443 if secure else 80)
        raw = socket.create_connection((p.hostname, port))
        self.sock = ssl.create_default_context().wrap_socket(raw, server_hostname=p.hostname) if secure else raw
        key = base64.b64encode(os.urandom(16)).decode()
        path = (p.path or "/") + ("?" + p.query if p.query else "")
        host = p.hostname + ("" if p.port is None else ":%d" % p.port)
        self.sock.sendall(("GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (path, host, key)).encode())
        self.buf = b""
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk: raise EOFError("connection closed during the handshake")
            self.buf += chunk
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        status = head.split(b"\r\n")[0].decode("latin-1")
        if " 101 " not in status: raise RuntimeError("the referee refused the connection: " + status)
    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk: raise EOFError("connection closed")
            self.buf += chunk
        data, self.buf = self.buf[:n], self.buf[n:]
        return data
    def _frame(self, opcode, payload):
        n = len(payload); head = bytes([0x80 | opcode])
        head += bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n) if n < 65536 else bytes([0x80 | 127]) + struct.pack(">Q", n)
        mask = os.urandom(4)                                    # a client masks every frame
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))
    def send(self, text): self._frame(0x1, text.encode("utf-8"))
    def receive(self):
        """The next text message, or None once the other side closed."""
        msg = b""
        while True:
            b0, b1 = self._read(2); opcode, n = b0 & 0x0F, b1 & 0x7F
            if n == 126: n = struct.unpack(">H", self._read(2))[0]
            elif n == 127: n = struct.unpack(">Q", self._read(8))[0]
            mask = self._read(4) if b1 & 0x80 else None
            payload = self._read(n)
            if mask: payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
            if opcode == 0x8: return None
            if opcode == 0x9: self._frame(0xA, payload); continue
            if opcode in (0x0, 0x1):
                msg += payload
                if b0 & 0x80: return msg.decode("utf-8")
    def close(self):
        try: self._frame(0x8, struct.pack(">H", 1000)); self.sock.close()
        except OSError: pass

def main():
    ap = argparse.ArgumentParser(description="Connect CWOTR as a bot to a wotr-protocol referee over a WebSocket.")
    ap.add_argument("--connect", required=True, help="the referee's WebSocket address (ws:// or wss://)")
    ap.add_argument("--token", help="seat token or bot key; better: set CWOTR_TOKEN in the environment so it stays out of the shell history")
    ap.add_argument("--shadow", choices=["hunter", "conquest"], default="hunter", help="which CWOTR Shadow plays")
    ap.add_argument("--program", default=os.path.join(HERE, "cwotr-referee"), help="the CWOTR program (default: next to this script)")
    ap.add_argument("--player", help="the player files to use (default: players/cwotr1 or cwotr1-conquest next to this script)")
    ap.add_argument("--log", default="cwotr-bot.log", help="where CWOTR's own notes on its games go")
    a = ap.parse_args()
    token = a.token or os.environ.get("CWOTR_TOKEN")
    url = a.connect
    if token and "token=" not in url: url += ("&" if "?" in url else "?") + "token=" + token
    player = a.player or os.path.join(HERE, "players", "cwotr1" if a.shadow == "hunter" else "cwotr1-conquest")
    env = {k: v for k, v in os.environ.items() if k != "CWOTR_TOKEN"}
    with open(a.log, "a") as botlog:
        bot = subprocess.Popen([a.program, "bot", player], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=botlog, env=env, text=True, bufsize=1)
        ws = WebSocket(url)
        log("connected to", a.connect.split("?")[0])
        games = 0
        try:
            while True:
                text = ws.receive()
                if text is None: log("the referee closed the connection"); break
                for line in text.splitlines():
                    if not line.strip(): continue
                    try: kind = json.loads(line).get("type")
                    except ValueError: log("not JSON:", line[:120]); continue
                    if kind == "error": log("the referee says:", line[:300])
                    bot.stdin.write(line + "\n"); bot.stdin.flush()
                    if kind in ("hello", "new_game", "decide"):
                        answer = bot.stdout.readline()
                        if not answer: raise EOFError("CWOTR's player stopped; see " + a.log)
                        ws.send(answer.strip())
                    if kind == "game_over":
                        games += 1; m = json.loads(line); log("game %d over: winner %s by %s on turn %s" % (games, m.get("winner"), m.get("how"), m.get("turn")))
                    if kind == "quit": raise SystemExit
        except SystemExit:
            pass
        except EOFError as e:
            log("the connection ended:", e)
        finally:
            ws.close()
            try: bot.stdin.close(); bot.wait(timeout=5)
            except Exception: bot.kill()

if __name__ == "__main__":
    main()
