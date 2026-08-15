#!/usr/bin/env python3
#
# Copyright 2026 Toyota Connected North America
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Live view of the semantics tree an ivi-homescreen shell is serving.

Holds one event stream open and redraws whenever the UI changes, so what is
on screen and what an agent can see sit side by side. Nothing is polled: the
redraw is driven by notifications/resources/updated arriving on the stream.

Standard library only, so it runs on a board with nothing installed.

    scripts/mcp_inspector.py [--socket PATH] [--once]

The shell must be built with BUILD_MCP and started with [global] enable_mcp;
the socket defaults to $XDG_RUNTIME_DIR/ivi-homescreen/mcp.sock.

To watch a board from a workstation, forward the socket over ssh rather than
exposing a port -- the shell deliberately serves no TCP:

    ssh -L /tmp/ivi-mcp.sock:/run/user/1000/ivi-homescreen/mcp.sock user@board
    scripts/mcp_inspector.py --socket /tmp/ivi-mcp.sock
"""

import argparse
import json
import os
import socket
import sys
import time

# The tree URIs are discovered rather than hardcoded: there is one per running
# application, named for it, and which are present depends on what the shell
# was configured with. A client that assumes a single fixed URI works until a
# second application appears and then reads the wrong one, or nothing.
TREE_URI_PREFIX = "ui://semantics/"

# Roles worth telling apart at a glance. Anything unlisted renders plain,
# which keeps an unfamiliar role visible rather than mis-coloured.
ROLE_COLOR = {
    "window": "\033[95m",
    "button": "\033[92m",
    "text_input": "\033[96m",
    "multiline_text_input": "\033[96m",
    "password_input": "\033[91m",
    "slider": "\033[93m",
    "switch": "\033[93m",
    "check_box": "\033[93m",
    "radio_button": "\033[93m",
    "link": "\033[94m",
    "heading": "\033[95m",
    "scroll_view": "\033[90m",
    "pane": "\033[90m",
    "generic_container": "\033[90m",
}
RESET = "\033[0m"
DIM = "\033[90m"
BOLD = "\033[1m"
CHANGED = "\033[43;30m"  # a node whose content moved since the last update


class ServerGone(Exception):
    """The shell closed the connection or was never there."""


def connect(path):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.connect(path)
    except (FileNotFoundError, ConnectionRefusedError) as exc:
        sock.close()
        raise ServerGone(f"no MCP server at {path}: {exc}") from exc
    return sock


def rpc(path, method, params=None, request_id=1):
    """One JSON-RPC call. A new connection each time: the request side of the
    transport answers with Connection: close, which is what a request/response
    exchange should do."""
    body = {"jsonrpc": "2.0", "id": request_id, "method": method}
    if params is not None:
        body["params"] = params
    payload = json.dumps(body).encode()

    sock = connect(path)
    try:
        sock.sendall(
            b"POST /mcp HTTP/1.1\r\n"
            b"Content-Type: application/json\r\n"
            b"Content-Length: " + str(len(payload)).encode() + b"\r\n\r\n"
            + payload
        )
        raw = b""
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            raw += chunk
    finally:
        sock.close()

    _, _, text = raw.partition(b"\r\n\r\n")
    if not text:
        raise ServerGone("server closed without answering")
    reply = json.loads(text)
    if "error" in reply:
        raise ServerGone(f"{method}: {reply['error'].get('message')}")
    return reply["result"]


def tree_uris(path):
    """Every semantics tree the server is serving, in listed order."""
    result = rpc(path, "resources/list", {}, request_id=2)
    return [
        r["uri"]
        for r in result.get("resources", [])
        if r.get("uri", "").startswith(TREE_URI_PREFIX)
    ]


def read_tree(path, uri=None):
    """One application's tree. With no uri, the only one -- and it says so
    rather than guessing when there are several, because picking one would
    silently show a different application than the caller meant."""
    if uri is None:
        found = tree_uris(path)
        if not found:
            raise ServerGone("no application is publishing a UI")
        if len(found) > 1:
            names = ", ".join(found)
            raise ServerGone(f"several applications are running; name one: {names}")
        uri = found[0]
    result = rpc(path, "resources/read", {"uri": uri}, request_id=3)
    return json.loads(result["contents"][0]["text"])


def open_stream(path):
    """Opens the server-to-client half and returns the socket, having consumed
    the response headers."""
    sock = connect(path)
    sock.sendall(
        b"GET /mcp HTTP/1.1\r\nAccept: text/event-stream\r\n\r\n"
    )
    header = b""
    while b"\r\n\r\n" not in header:
        chunk = sock.recv(1024)
        if not chunk:
            sock.close()
            raise ServerGone("server refused the event stream")
        header += chunk
    if b"text/event-stream" not in header:
        sock.close()
        status = header.split(b"\r\n", 1)[0].decode(errors="replace")
        raise ServerGone(f"server did not open a stream: {status}")
    return sock, header.partition(b"\r\n\r\n")[2]


def events(sock, pending):
    """Yields one decoded event per SSE frame, forever."""
    buffer = pending
    while True:
        while b"\n\n" in buffer:
            frame, _, buffer = buffer.partition(b"\n\n")
            for line in frame.replace(b"\r\n", b"\n").split(b"\n"):
                if line.startswith(b"data: "):
                    yield json.loads(line[6:])
        chunk = sock.recv(65536)
        if not chunk:
            raise ServerGone("the shell closed the stream")
        buffer += chunk


def summarize(node):
    """The compact right-hand column: what this node is and what can be done
    to it. States are only shown when they apply -- a tristate that does not
    apply is absent rather than false, and flattening that would make every
    container look like an unchecked checkbox."""
    bits = []
    value = node.get("value") or ""
    if value:
        bits.append(f"= {value}")
    state = node.get("state") or {}
    # "not_applicable" is the tristate saying the property does not apply to
    # this node, which is most properties on most nodes. Showing it would bury
    # the few that do apply -- and the distinction it encodes (a container is
    # not an unchecked checkbox) is only useful when it is legible.
    for key in ("checked", "toggled", "selected", "expanded", "focused"):
        value_ = state.get(key)
        if value_ not in (None, "not_applicable", "none", "false"):
            bits.append(f"{key}:{value_}")
    if state.get("enabled") == "false":
        bits.append("disabled")
    for flag in ("hidden", "read_only"):
        if state.get(flag):
            bits.append(flag)
    identifier = node.get("identifier") or ""
    if identifier:
        bits.append(f"#{identifier}")
    return "  ".join(bits)


def fingerprint(node):
    """What has to change for a node to count as changed. Deliberately not the
    whole node: rects move on every frame of an animation, and highlighting
    the entire tree during a transition tells you nothing."""
    state = node.get("state") or {}
    return (
        node.get("label"),
        node.get("value"),
        node.get("role"),
        tuple(sorted(node.get("actions") or [])),
        tuple(sorted((k, str(v)) for k, v in state.items())),
    )


def render(tree, previous, width, event_count, last_event,
           server_name="", tools_changed=0):
    by_id = {n["id"]: n for n in tree.get("nodes", [])}
    lines = []

    def walk(node_id, depth):
        node = by_id.get(node_id)
        if node is None:
            return
        role = node.get("role", "unknown")
        label = node.get("label") or ""
        color = ROLE_COLOR.get(role, "")
        changed = (
            previous is not None
            and node_id in previous
            and previous[node_id] != fingerprint(node)
        )
        added = previous is not None and node_id not in previous

        marker = " "
        if added:
            marker = "+"
        elif changed:
            marker = "~"

        indent = "  " * depth
        head = f"{marker} {indent}{color}{role}{RESET}"
        if label:
            head += f" {BOLD}{label}{RESET}"
        detail = summarize(node)
        actions = node.get("actions") or []
        custom = [c.get("label", "") for c in (node.get("custom_actions") or [])]

        line = head
        if detail:
            line += f"  {detail}"
        if actions:
            line += f"  {DIM}[{' '.join(actions)}]{RESET}"
        if custom:
            line += f"  {DIM}<{' | '.join(custom)}>{RESET}"
        if changed or added:
            # Marked rather than fully highlighted: the eye needs to find the
            # change, not lose the tree around it.
            line = f"{CHANGED}{marker}{RESET}" + line[1:]
        lines.append(line[: width + 200])  # escape codes do not count as width

        for child in node.get("children") or []:
            walk(child, depth + 1)

    nodes = tree.get("nodes", [])
    if nodes:
        walk(nodes[0]["id"], 0)
    else:
        lines.append(f"{DIM}  (no nodes -- the app has published an empty tree){RESET}")

    generation = tree.get("generation", "?")
    header = (
        f"{BOLD}{server_name or 'ivi-homescreen'} semantics{RESET}  "
        f"generation {generation}  ·  {len(nodes)} nodes  ·  "
        f"{event_count} updates"
    )
    if tools_changed:
        header += f"  ·  {tools_changed} tool-list changes"
    if last_event:
        header += f"  ·  last {time.strftime('%H:%M:%S', time.localtime(last_event))}"

    sys.stdout.write("\033[H\033[2J")
    sys.stdout.write(header + "\n")
    sys.stdout.write(DIM + "-" * min(width, 100) + RESET + "\n")
    sys.stdout.write("\n".join(lines) + "\n")
    sys.stdout.write(
        f"\n{DIM}+ added   ~ changed   [action] drivable   <custom action>"
        f"   ctrl-c to exit{RESET}\n"
    )
    sys.stdout.flush()


def default_socket():
    runtime = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
    return os.path.join(runtime, "ivi-homescreen", "mcp.sock")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--socket", default=default_socket(),
                        help="MCP socket path (default: %(default)s)")
    parser.add_argument("--once", action="store_true",
                        help="print the tree once and exit, for scripting")
    args = parser.parse_args()

    try:
        width = os.get_terminal_size().columns
    except OSError:
        width = 100

    try:
        if args.once:
            tree = read_tree(args.socket)
            render(tree, None, width, 0, None)
            return 0

        # The stream is opened before the first read, so a change landing
        # between the two is delivered rather than missed.
        stream, pending = open_stream(args.socket)
        server = rpc(args.socket, "initialize", {}, request_id=1)
        name = server.get("serverInfo", {}).get("name", "server")

        tree = read_tree(args.socket)
        previous = {n["id"]: fingerprint(n) for n in tree.get("nodes", [])}
        render(tree, None, width, 0, None, name)

        count = 0
        tools_changed = 0
        for event in events(stream, pending):
            method = event.get("method", "")
            if method == "notifications/tools/list_changed":
                # Counted, not redrawn on. A provider signals once for either
                # kind of change and the server emits both, so redrawing here
                # too would repaint every update twice -- and the second paint
                # would show no change, making the first one flicker away.
                tools_changed += 1
                continue
            if method != "notifications/resources/updated":
                continue
            count += 1
            tree = read_tree(args.socket)
            render(tree, previous, width, count, time.time(), name,
                   tools_changed)
            previous = {n["id"]: fingerprint(n) for n in tree.get("nodes", [])}
    except ServerGone as exc:
        sys.stderr.write(f"\n{exc}\n")
        return 1
    except KeyboardInterrupt:
        sys.stdout.write("\n")
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
