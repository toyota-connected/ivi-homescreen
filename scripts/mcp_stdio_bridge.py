#!/usr/bin/env python3
"""Bridges an MCP client that speaks stdio to ivi-homescreen's Unix socket.

The shell serves MCP as JSON-RPC over HTTP on a Unix domain socket, which is
the right shape for the shell -- the socket's permissions are the access
control -- but no general MCP client dials a Unix socket. This is the adapter:
newline-delimited JSON-RPC on stdin and stdout, HTTP on the socket.

Register it with Claude Code:

    claude mcp add cockpit -- python3 /path/to/scripts/mcp_stdio_bridge.py

or add it to a Claude Desktop configuration:

    {"mcpServers": {"cockpit": {"command": "python3",
                                "args": ["/path/to/scripts/mcp_stdio_bridge.py"]}}}

Standard library only, so it runs wherever the shell does with nothing to
install.

Server-initiated notifications are forwarded too: the bridge opens the event
stream and relays what arrives on it, so a client learns that tools or the UI
changed rather than having to poll.
"""

import argparse
import json
import os
import socket
import sys
import threading

DEFAULT_SOCKET = os.path.join(
    os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}",
    "ivi-homescreen", "mcp.sock")

# stdout is the protocol channel, so every diagnostic goes to stderr. A stray
# print would corrupt the stream and the client would drop the connection.
def log(message):
    print(f"[mcp-bridge] {message}", file=sys.stderr, flush=True)


_stdout_lock = threading.Lock()


def emit(message):
    """Writes one JSON-RPC message to stdout.

    Locked because notifications arrive on the stream thread while replies are
    written from the main one, and two interleaved writes are not two
    messages.
    """
    line = json.dumps(message, separators=(",", ":"))
    with _stdout_lock:
        sys.stdout.write(line + "\n")
        sys.stdout.flush()


def connect(path):
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.connect(path)
    return client


def request(path, payload):
    """Sends one JSON-RPC message and returns the reply, or None.

    A connection per request: the shell answers with Connection: close, which
    is what a request/response transport over HTTP looks like.
    """
    body = json.dumps(payload, separators=(",", ":")).encode()
    head = (b"POST / HTTP/1.1\r\n"
            b"Content-Type: application/json\r\n"
            b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n")
    client = connect(path)
    try:
        client.sendall(head + body)
        chunks = []
        while True:
            chunk = client.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
    finally:
        client.close()

    raw = b"".join(chunks)
    split = raw.find(b"\r\n\r\n")
    if split < 0:
        return None
    status = raw.split(b"\r\n", 1)[0].decode(errors="replace")
    payload = raw[split + 4:]
    if not payload:
        # 202: a notification was accepted and there is nothing to answer.
        if " 202 " not in status:
            log(f"empty reply to a request that expected one: {status}")
        return None
    try:
        return json.loads(payload)
    except json.JSONDecodeError:
        log(f"reply was not JSON ({status}): {payload[:200]!r}")
        return None


def pump_events(path):
    """Relays server-initiated notifications from the event stream.

    Runs until the stream ends, which happens when the shell stops. The bridge
    keeps serving requests either way -- a client that loses notifications can
    still poll, and killing the session over it would be worse.
    """
    try:
        client = connect(path)
    except OSError as error:
        log(f"no event stream ({error}); notifications will not be forwarded")
        return

    try:
        client.sendall(b"GET / HTTP/1.1\r\nAccept: text/event-stream\r\n\r\n")
        buffer = b""
        seen_headers = False
        while True:
            chunk = client.recv(65536)
            if not chunk:
                break
            buffer += chunk
            if not seen_headers:
                split = buffer.find(b"\r\n\r\n")
                if split < 0:
                    continue
                seen_headers = True
                buffer = buffer[split + 4:]
            # Server-sent events: "data: <json>" terminated by a blank line.
            while b"\n\n" in buffer:
                frame, buffer = buffer.split(b"\n\n", 1)
                for line in frame.splitlines():
                    if not line.startswith(b"data:"):
                        continue
                    try:
                        emit(json.loads(line[5:].strip()))
                    except json.JSONDecodeError:
                        log(f"bad event frame: {line[:200]!r}")
    except OSError as error:
        log(f"event stream ended: {error}")
    finally:
        client.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--socket", default=DEFAULT_SOCKET,
                        help="MCP socket path (default: %(default)s)")
    parser.add_argument("--no-events", action="store_true",
                        help="do not forward server notifications")
    args = parser.parse_args()

    if not os.path.exists(args.socket):
        # Said plainly, because this is the failure everyone hits first: the
        # shell must be built with the MCP surface and started with it on.
        log(f"no socket at {args.socket}")
        log("start ivi-homescreen with --enable-mcp, or pass --socket")
        return 1

    if not args.no_events:
        threading.Thread(target=pump_events, args=(args.socket,),
                         daemon=True).start()

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except json.JSONDecodeError as error:
            emit({"jsonrpc": "2.0", "id": None,
                  "error": {"code": -32700, "message": f"parse error: {error}"}})
            continue

        try:
            reply = request(args.socket, message)
        except OSError as error:
            # Answered rather than dropped: a client waiting on an id that
            # never comes back hangs, which is worse than a reported failure.
            if isinstance(message, dict) and "id" in message:
                emit({"jsonrpc": "2.0", "id": message["id"],
                      "error": {"code": -32603,
                                "message": f"the shell is not reachable: {error}"}})
            else:
                log(f"send failed: {error}")
            continue

        if reply is not None:
            emit(reply)
    return 0


if __name__ == "__main__":
    sys.exit(main())
