"""Minimal real CDP WebSocket client for private integration tests (stdlib only)."""
import base64
import json
import os
import socket
import struct
import urllib.request
import urllib.parse

class Cdp:
    def __init__(self, endpoint):
        with urllib.request.urlopen(endpoint + "/json/version", timeout=4) as response:
            metadata = json.load(response)
        assert metadata["Browser"].startswith("Chrome/"), metadata
        url = urllib.parse.urlsplit(metadata["webSocketDebuggerUrl"])
        self.socket = socket.create_connection((url.hostname, url.port), timeout=5)
        nonce = base64.b64encode(os.urandom(16)).decode()
        self.socket.sendall((f"GET {url.path} HTTP/1.1\r\nHost: {url.netloc}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {nonce}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        answer = b""
        while b"\r\n\r\n" not in answer:
            chunk = self.socket.recv(1)
            assert chunk, answer
            answer += chunk
        assert answer.startswith(b"HTTP/1.1 101"), answer
        self.serial = 0
        self.events = []
    def exact(self, size):
        data = b""
        while len(data) < size:
            chunk = self.socket.recv(size - len(data))
            if not chunk: raise ConnectionError("CDP disconnected")
            data += chunk
        return data
    def call(self, method, params=None, session=None):
        self.serial += 1
        message = {"id": self.serial, "method": method, "params": params or {}}
        if session: message["sessionId"] = session
        data = json.dumps(message).encode(); mask = os.urandom(4)
        length = len(data)
        if length < 126: header = bytes([0x81, 0x80 | length])
        elif length < 65536: header = bytes([0x81, 0x80 | 126]) + struct.pack('!H', length)
        else: header = bytes([0x81, 0x80 | 127]) + struct.pack('!Q', length)
        self.socket.sendall(header + mask + bytes(c ^ mask[i % 4] for i, c in enumerate(data)))
        while True:
            flags, size = self.exact(2); length = size & 127
            if length == 126: length = struct.unpack('!H', self.exact(2))[0]
            elif length == 127: length = struct.unpack('!Q', self.exact(8))[0]
            payload = self.exact(length)
            if flags & 15 == 8: raise ConnectionError("CDP authorization revoked")
            if flags & 15 != 1: continue
            reply = json.loads(payload)
            if reply.get('id') != self.serial:
                if reply.get('method') in ('Network.loadingFailed', 'Network.responseReceived'): self.events.append(reply)
                continue
            assert 'error' not in reply, reply
            return reply.get('result', {})
    def close(self): self.socket.close()
