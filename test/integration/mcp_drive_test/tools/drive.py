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
"""Drives mcp_drive_test over MCP and checks what the app actually did.

Every other MCP test runs against a mock host and can only show that a
request reached the shell. This one runs against a real engine, so it checks
the things that are only true end to end -- that a dispatch invokes the
handler the widget registered, that ui_set_text reaches a field's editing
connection, and that ui_tap_at reaches a target the tree does not describe
while ui_tap cannot.

Results are read back through MCP: the app writes every handler's outcome
into a status node, so the read path is the assertion channel for the write
path and neither can pass alone.

    tools/drive.py [--socket PATH] [--timeout SECONDS]

Exits non-zero on the first failed check, listing every result.
"""

import argparse
import json
import os
import socket
import sys
import time

TREE_URI = "ui://semantics/tree"


class Failure(Exception):
    pass


def rpc(path, method, params=None, request_id=1):
    body = {"jsonrpc": "2.0", "id": request_id, "method": method}
    if params is not None:
        body["params"] = params
    payload = json.dumps(body).encode()

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.connect(path)
    except OSError as exc:
        raise Failure(f"no MCP server at {path}: {exc}") from exc
    try:
        sock.sendall(
            b"POST /mcp HTTP/1.1\r\nContent-Length: "
            + str(len(payload)).encode()
            + b"\r\n\r\n"
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
        raise Failure(f"{method}: server closed without answering")
    return json.loads(text)


def call_tool(path, name, arguments=None, request_id=1):
    """Returns (ok, document). A tool that refuses is a result, not a crash:
    several checks here assert precisely that something was refused."""
    reply = rpc(path, "tools/call",
                {"name": name, "arguments": arguments or {}}, request_id)
    if "error" in reply:
        return False, reply["error"]
    content = reply["result"]["content"][0]["text"]
    return not reply["result"].get("isError", False), json.loads(content)


def read_tree(path):
    reply = rpc(path, "resources/read", {"uri": TREE_URI}, request_id=2)
    if "error" in reply:
        raise Failure(f"resources/read: {reply['error']}")
    return json.loads(reply["result"]["contents"][0]["text"])


def find(tree, label=None, role=None, identifier=None):
    """Finds a node by label and role, preferring an identifier when the
    engine reports one.

    Label and role are the primary key on purpose. Flutter 3.38.3 -- the
    deployment floor this port targets (plan section 2.2) -- never writes
    `identifier`, so a fixture keyed on it would pass on a development engine
    and fail on every shipping target. The app still sets identifiers, which
    a newer engine will use for a more exact match.
    """
    if identifier:
        for node in tree.get("nodes", []):
            if node.get("identifier") == identifier:
                return node
    for node in tree.get("nodes", []):
        if label is not None and node.get("label") != label:
            continue
        if role is not None and node.get("role") != role:
            continue
        return node
    return None


def status_fields(path):
    """The app's status node, parsed into a dict. Values are space separated
    key=value pairs, which survives a round trip through a semantics value
    without needing the app to embed JSON in a label."""
    tree = read_tree(path)
    node = find(tree, label="status", identifier="status")
    if node is None:
        raise Failure("the app published no status node; is it the right app?")
    # Values may contain spaces -- set_text is given a name with one on
    # purpose -- so the app separates fields with semicolons.
    fields = {}
    for part in (node.get("value") or "").split(";"):
        key, _, value = part.strip().partition("=")
        if key:
            fields[key] = value
    return fields


def wait_for(path, predicate, timeout, what):
    """Polls the status node until the app reports what was expected.

    Polled rather than driven by a notification because the check is about a
    widget handler having run, which is one frame later than the dispatch the
    server acknowledged -- and a test that raced that would fail
    intermittently for a reason unrelated to what it covers.
    """
    deadline = time.time() + timeout
    last = {}
    while time.time() < deadline:
        last = status_fields(path)
        if predicate(last):
            return last
        time.sleep(0.05)
    raise Failure(f"timed out waiting for {what}; status was {last}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    default_runtime = os.environ.get("XDG_RUNTIME_DIR") or \
        f"/run/user/{os.getuid()}"
    parser.add_argument(
        "--socket",
        default=os.path.join(default_runtime, "ivi-homescreen", "mcp.sock"),
        help="MCP socket path (default: %(default)s)")
    parser.add_argument("--timeout", type=float, default=5.0,
                        help="seconds to wait for the app to react")
    args = parser.parse_args()

    results = []

    def check(name, description, fn):
        try:
            fn()
        except Failure as exc:
            results.append((name, description, False, str(exc)))
        except Exception as exc:  # noqa: BLE001 - a driver bug is a failure too
            results.append((name, description, False, f"{type(exc).__name__}: {exc}"))
        else:
            results.append((name, description, True, ""))

    # C1 -- the tree describes the app. Everything after this depends on it,
    # so it is checked first and separately: an empty tree means the app is
    # not annotated or semantics never came up, which is a different problem
    # from a dispatch not working.
    def c1():
        tree = read_tree(args.socket)
        for label, role in (("status", None), ("Save", "button"),
                            ("Locked", "button"), ("Destination", None)):
            if find(tree, label=label, role=role) is None:
                raise Failure(f"no node labelled {label!r}"
                              + (f" with role {role}" if role else ""))
        save = find(tree, label="Save", role="button", identifier="save")
        if "tap" not in (save.get("actions") or []):
            raise Failure(f"Save offers no tap action: {save.get('actions')}")

    # C2 -- a dispatch invokes the widget's own handler. This is the claim the
    # mock-host tests cannot make: they end where the shell begins.
    def c2():
        before = status_fields(args.socket)
        node = find(read_tree(args.socket), label="Save", role="button",
                    identifier="save")
        ok, doc = call_tool(args.socket, "ui_tap", {"node_id": node["id"]}, 10)
        if not ok:
            raise Failure(f"ui_tap refused: {doc}")
        after = wait_for(
            args.socket,
            lambda f: f.get("last") == "tap:save"
            and f.get("events") != before.get("events"),
            args.timeout, "the Save handler to run")
        del after

    # C3 -- plan R-9's exit criterion: the text actually lands in the field.
    def c3():
        node = find(read_tree(args.socket), label="Destination",
                    identifier="destination")
        ok, doc = call_tool(args.socket, "ui_set_text",
                            {"node_id": node["id"], "text": "Sarah Connor"}, 11)
        if not ok:
            raise Failure(f"ui_set_text refused: {doc}")
        wait_for(args.socket,
                 lambda f: f.get("text") == "Sarah Connor",
                 args.timeout, "the field to take the new text")

    # C4 -- the DR-7 contrast, in both directions.
    def c4_no_node():
        tree = read_tree(args.socket)
        for node in tree.get("nodes", []):
            if (node.get("label") or "") == "unannotated region":
                raise Failure("the excluded region appeared in the tree; "
                              "ExcludeSemantics is not doing its job and the "
                              "contrast this fixture exists to show is gone")

    def c4_pointer_reaches():
        fields = status_fields(args.socket)
        geometry = fields.get("opaque", "")
        try:
            left, top, width, height = (float(v) for v in geometry.split(","))
        except ValueError as exc:
            raise Failure(f"app did not report its geometry: {geometry!r}") from exc
        before = status_fields(args.socket)
        ok, doc = call_tool(args.socket, "ui_tap_at",
                            {"x": left + width / 2, "y": top + height / 2}, 12)
        if not ok:
            raise Failure(f"ui_tap_at refused: {doc}")
        wait_for(
            args.socket,
            lambda f: f.get("last") == "tap:opaque"
            and f.get("events") != before.get("events"),
            args.timeout, "the unannotated region to receive a pointer tap")

    # C5 -- a disabled control is refused rather than dispatched into silence.
    def c5():
        node = find(read_tree(args.socket), label="Locked", role="button",
                    identifier="locked")
        before = status_fields(args.socket)
        ok, doc = call_tool(args.socket, "ui_tap", {"node_id": node["id"]}, 13)
        if ok:
            raise Failure("ui_tap on a disabled control was accepted")
        time.sleep(0.2)
        after = status_fields(args.socket)
        if after.get("events") != before.get("events"):
            raise Failure("a refused tap still reached the widget")

    check("C1", "the tree describes the app", c1)
    check("C2", "ui_tap invokes the widget's own handler", c2)
    check("C3", "ui_set_text reaches the field (R-9)", c3)
    check("C4a", "the unannotated region is absent from the tree", c4_no_node)
    check("C4b", "ui_tap_at reaches it anyway (DR-7)", c4_pointer_reaches)
    check("C5", "a disabled control is refused before dispatch", c5)

    width = max(len(d) for _, d, _, _ in results)
    failed = 0
    for name, description, ok, detail in results:
        mark = "PASS" if ok else "FAIL"
        print(f"{name:4s} {description:<{width}s}  {mark}")
        if not ok:
            failed += 1
            print(f"       {detail}")
    print()
    print(f"{len(results) - failed}/{len(results)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Failure as exc:
        sys.stderr.write(f"{exc}\n")
        sys.exit(1)
