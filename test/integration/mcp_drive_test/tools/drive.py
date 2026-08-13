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
import re
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


def query(path, **selectors):
    """ui_query with any of its selectors. Returns the decoded result."""
    ok, doc = call_tool(path, "ui_query", selectors, request_id=7)
    if not ok:
        raise Failure(f"ui_query {selectors}: {doc}")
    return doc


def act(path, tool, arguments, what):
    """Invokes an action tool and returns its verify-after-act result.

    The point of DR-8 is that this is enough on its own: the caller does not
    re-read the tree to find out what happened, because the response already
    carries the node as it stands afterwards.
    """
    ok, doc = call_tool(path, tool, arguments, request_id=8)
    if not ok:
        raise Failure(f"{tool} ({what}) was refused: {doc}")
    if not doc.get("dispatched"):
        raise Failure(f"{tool} ({what}) was not dispatched: {doc}")
    return doc


def numeric(value):
    """The leading number in a slider's reported value, or None.

    The framework formats this itself and the exact spelling is not ours to
    depend on -- percentages and units both occur -- so this reads the number
    and ignores the rest rather than matching a string.
    """
    if value is None:
        return None
    match = re.search(r"-?\d+(?:\.\d+)?", str(value))
    return float(match.group()) if match else None


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

    # C6 -- a multi-step flow driven entirely by what
    # an agent can actually see, with each step verified from the action's own
    # response rather than by re-reading the tree.
    #
    # No node ids are carried in from outside and no identifiers are used:
    # identifiers are empty at the 3.38.3 deployment floor (plan section 2.2),
    # so a flow that depended on them would pass here and fail on a target.
    def c6_multi_step_flow():
        # Step 1: find the control by role and the action it offers, which is
        # how an agent picks a target it was not told about.
        found = query(args.socket, role="slider", action="increase")
        if found.get("match_count", 0) < 1:
            raise Failure("no slider offering increase; the app did not "
                          "publish one, or the selectors did not match it")
        slider = found["nodes"][0]
        if slider.get("label") != "Temperature":
            raise Failure(f"found the wrong slider: {slider.get('label')!r}")

        start = numeric(slider.get("value"))
        if start is None:
            raise Failure(f"slider reports no numeric value: {slider!r}")
        app_start = numeric(status_fields(args.socket).get("temp"))
        if app_start is None:
            raise Failure("the app did not report its temperature")

        # Step 2: act, and read the outcome out of the action's own reply.
        first = act(args.socket, "ui_increase", {"node_id": slider["id"]},
                    "raise the temperature")
        if first.get("mode") != "semantics":
            raise Failure(f"expected a semantics dispatch: {first}")
        after = first.get("node_after")
        if after is None:
            raise Failure("node_after was null; the slider left the tree")
        raised = numeric(after.get("value"))
        if raised is None or raised <= start:
            raise Failure(
                f"the slider did not move: {start} -> {raised}. The action "
                f"reached the framework, so either the widget ignored it or "
                f"node_after is reporting the pre-action tree")

        # Step 3: again, to show the flow accumulates rather than each step
        # being read against a stale baseline.
        second = act(args.socket, "ui_increase", {"node_id": slider["id"]},
                     "raise it again")
        twice = numeric((second.get("node_after") or {}).get("value"))
        if twice is None or twice <= raised:
            raise Failure(f"second step did not advance: {raised} -> {twice}")

        # Step 4: a custom action, found and invoked by its label. These are
        # the application's own verbs -- there is no fixed set, and the label
        # is the only handle an agent has on one.
        climate = query(args.socket, custom_action="Set to Auto")
        if climate.get("match_count", 0) < 1:
            raise Failure("no node declares the custom action 'Set to Auto'")
        node = climate["nodes"][0]
        act(args.socket, "ui_custom_action",
            {"node_id": node["id"], "action": "Set to Auto"},
            "switch the climate mode")

        # The custom action's effect is app state rather than node state, so
        # this one step is confirmed through the status line -- which is also
        # the proof the widget's own closure ran, not merely that the dispatch
        # was accepted.
        fields = wait_for(args.socket, lambda f: f.get("mode") == "auto",
                          args.timeout, "the custom action's handler to run")

        # And the slider's movement reached the app's own state, not just the
        # semantics node.
        #
        # Compared as a direction, not a number. The framework reports a
        # slider's semantic value as a percentage of its range while the app
        # holds degrees, so the two are the same movement in different units
        # and equating them would be checking the unit conversion rather than
        # the dispatch.
        reported = numeric(fields.get("temp"))
        if reported is None or reported <= app_start:
            raise Failure(
                f"the tree moved but the app did not: app was {app_start}, "
                f"now {reported}, while node_after went {start} -> {twice}")

    # C7 -- a tool the application declared for itself, with a schema. The
    # semantics verbs cannot express this: an agent names the value it wants
    # instead of working out how many increments reach it.
    def c7_app_declared_tool():
        listed = rpc(args.socket, "tools/list", request_id=20)
        if "error" in listed:
            raise Failure(f"tools/list: {listed['error']}")
        tools = {t["name"]: t for t in listed["result"]["tools"]}
        declared = tools.get("fixture_set_temp")
        if declared is None:
            raise Failure(
                "the application's tool was not advertised; it is registered "
                f"from the app, so this means the FFI registration failed. "
                f"Saw: {sorted(tools)}")

        # The schema is the point of a declared tool -- it is what tells an
        # agent the argument is a number in a range rather than a label to
        # match.
        schema = json.dumps(declared.get("inputSchema", {}))
        if "celsius" not in schema:
            raise Failure(f"the declared schema lost its arguments: {schema}")

        ok, doc = call_tool(args.socket, "fixture_set_temp",
                            {"celsius": 24.5}, 21)
        if not ok:
            raise Failure(f"fixture_set_temp was refused: {doc}")

        # Confirmed through the app's own status line, so this shows the Dart
        # handler ran rather than the call being accepted somewhere in between.
        fields = wait_for(
            args.socket,
            lambda f: f.get("last") == "tool:set_temp",
            args.timeout, "the application's tool handler to run")
        reported = numeric(fields.get("temp"))
        if reported is None or abs(reported - 24.5) > 0.01:
            raise Failure(
                f"the handler ran but the value did not land: {reported}")

        # A tool that runs and fails is distinct from one that does not exist.
        refused, _ = call_tool(args.socket, "fixture_set_temp", {}, 22)
        if refused:
            raise Failure("a call with no celsius was accepted")
        missing, _ = call_tool(args.socket, "fixture_nonexistent", {}, 23)
        if missing:
            raise Failure("a call to a tool that does not exist was accepted")

    check("C1", "the tree describes the app", c1)
    check("C2", "ui_tap invokes the widget's own handler", c2)
    check("C3", "ui_set_text reaches the field (R-9)", c3)
    check("C4a", "the unannotated region is absent from the tree", c4_no_node)
    check("C4b", "ui_tap_at reaches it anyway (DR-7)", c4_pointer_reaches)
    check("C5", "a disabled control is refused before dispatch", c5)
    check("C6", "a multi-step flow by role, label and custom-action name",
          c6_multi_step_flow)
    check("C7", "a tool the application declared, with its schema",
          c7_app_declared_tool)

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
