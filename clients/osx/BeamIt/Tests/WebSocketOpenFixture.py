"""Local-only native upgrade regression fixture; Python standard library.

Generates a disposable TLS identity with macOS /usr/bin/openssl, launches the
compiled Swift runner, and removes the identity on exit. No host credentials,
clipboard, external connections, or dependencies are used. /download deliberately
never replies to ping: readiness must not depend on a pong, irrespective of how
Foundation buffers data. This is a fault fixture, not the deployed Beast host.
"""

import asyncio
import base64
import contextlib
import hashlib
from pathlib import Path
import ssl
import struct
import subprocess
import sys
import tempfile
from urllib.parse import parse_qs, urlsplit


def frame(opcode, payload):
    size = len(payload)
    if size < 126:
        header = struct.pack("!BB", 0x80 | opcode, size)
    elif size <= 65535:
        header = struct.pack("!BBH", 0x80 | opcode, 126, size)
    else:
        header = struct.pack("!BBQ", 0x80 | opcode, 127, size)
    return header + payload


async def read_frame(reader):
    first, second = await reader.readexactly(2)
    size = second & 127
    if size == 126:
        size = struct.unpack("!H", await reader.readexactly(2))[0]
    elif size == 127:
        size = struct.unpack("!Q", await reader.readexactly(8))[0]
    if not second & 128 or size > 262184 or not first & 128:
        raise ValueError("invalid test-client frame")
    mask = await reader.readexactly(4)
    data = await reader.readexactly(size)
    return first & 15, bytes(value ^ mask[index % 4] for index, value in enumerate(data))


async def messages(reader, writer, echo=False):
    while True:
        opcode, data = await read_frame(reader)
        if opcode == 8:
            return
        if echo and opcode == 2:
            writer.write(frame(2, data))
            await writer.drain()
        # Ping is deliberately not answered on this fault-injected peer.


def bulk_endpoint(target):
    """Validate synthetic bulk credentials, then select the fixture behavior."""
    url = urlsplit(target)
    encoded_id = url.path.removeprefix("/bulk/")
    try:
        raw = base64.urlsafe_b64decode(encoded_id + "==")
    except ValueError:
        return "/reject"
    token = base64.urlsafe_b64encode(bytes([0x5a]) * 32).decode().rstrip("=")
    canonical = base64.urlsafe_b64encode(raw).decode().rstrip("=")
    if len(raw) != 16 or encoded_id != canonical or parse_qs(url.query) != {"token": [token]}:
        return "/reject"
    modes = {0: "/download", 1: "/upload", 2: "/hang", 3: "/reject"}
    if raw != bytes([raw[0]]) * 16:
        return "/reject"
    return modes.get(raw[0], "/reject")


async def exchange(reader, writer):
    request = await reader.readuntil(b"\r\n\r\n")
    lines = request.decode("ascii").split("\r\n")
    method, path, _ = lines[0].split(" ")
    headers = dict(line.split(":", 1) for line in lines[1:] if ":" in line)
    headers = {key.lower(): value.strip() for key, value in headers.items()}
    if method != "GET" or "sec-websocket-key" not in headers:
        raise ValueError("invalid test upgrade")
    if path.startswith("/bulk/"):
        path = bulk_endpoint(path)
    if path == "/hang":
        await reader.read()  # TLS/HTTP received, but no upgrade; caller must cancel.
        return
    if path == "/reject":
        writer.write(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
        await writer.drain()
        return
    accept = base64.b64encode(hashlib.sha1(
        (headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")
    ).digest())
    writer.write(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                 b"Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n")
    await writer.drain()
    writer.transport.set_write_buffer_limits(high=64 * 1024, low=32 * 1024)
    if path == "/download":
        incoming = asyncio.create_task(messages(reader, writer))
        try:
            for index in range(32):
                payload = struct.pack("!I", index) + bytes([index]) * (262150 - 4)
                writer.write(frame(2, payload))
                await writer.drain()  # One owned message, not an 8 MiB application queue.
            await incoming
        finally:
            incoming.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await incoming
    elif path == "/upload":
        await messages(reader, writer, echo=True)
    elif path == "/noise":
        writer.write(frame(2, b"noise"))
        await writer.drain()
        await messages(reader, writer)
    else:
        raise ValueError("unknown fixture endpoint")


async def run(executable, certificate, key):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(certificate, key)
    failures = []
    connections = set()

    async def handle(reader, writer):
        owner = asyncio.current_task()
        connections.add(owner)
        try:
            await asyncio.wait_for(exchange(reader, writer), timeout=20)
        except (asyncio.IncompleteReadError, ConnectionError, ssl.SSLError):
            pass  # Expected native close, cancellation, rejection or wrong pin.
        except Exception as error:
            failures.append(error)
        finally:
            try:
                writer.close()
                with contextlib.suppress(ConnectionError, ssl.SSLError):
                    await writer.wait_closed()
            finally:
                connections.discard(owner)

    async with await asyncio.start_server(handle, "127.0.0.1", 0, ssl=context, limit=16384) as server:
        port = server.sockets[0].getsockname()[1]
        der = ssl.PEM_cert_to_DER_cert(Path(certificate).read_text())
        fingerprint = hashlib.sha256(der).hexdigest().upper()
        process = await asyncio.create_subprocess_exec(executable, f"wss://127.0.0.1:{port}", fingerprint)
        try:
            result = await asyncio.wait_for(process.wait(), timeout=40)
        except asyncio.TimeoutError:
            process.kill()  # Only the test child created above.
            await process.wait()
            raise RuntimeError("native regression exceeded 40-second fixture deadline")
        if result:
            raise RuntimeError(f"native runner exited {result}")
        if connections:
            await asyncio.wait_for(asyncio.gather(*tuple(connections)), timeout=5)
        if failures:
            raise RuntimeError(f"fixture failed: {failures!r}")
        print("PASS: local native upgrade fixture and Swift runner")


def main():
    if sys.platform != "darwin" or len(sys.argv) != 2:
        raise SystemExit("Run on macOS: python3 WebSocketOpenFixture.py /absolute/path/to/SwiftRunner")
    with tempfile.TemporaryDirectory(prefix="BeamIt-WebSocketOpen-") as temporary:
        certificate = str(Path(temporary) / "certificate.pem")
        key = str(Path(temporary) / "key.pem")
        subprocess.run(["/usr/bin/openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-days", "1", "-subj", "/CN=localhost", "-out", certificate, "-keyout", key],
                       check=True)
        asyncio.run(run(str(Path(sys.argv[1]).resolve()), certificate, key))


if __name__ == "__main__":
    main()
