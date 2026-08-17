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

# Discovered, not assumed: one tree per running application, named for it.
TREE_URI_PREFIX = "ui://semantics/"


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


def list_trees(path):
    """Every application's tree resource, sorted.

    The resources live under `result`, like every other JSON-RPC reply this
    driver reads. Taking them off the envelope instead finds nothing and
    reports it as "no application is running", which looks like a shell fault
    rather than a driver one -- so this reads the same place `resources/read`
    below reads.
    """
    listing = rpc(path, "resources/list", {}, request_id=2)
    if "error" in listing:
        raise Failure(f"resources/list: {listing['error']}")
    resources = listing.get("result", {}).get("resources", [])
    return sorted(
        r["uri"] for r in resources
        if r.get("uri", "").startswith(TREE_URI_PREFIX)
    )


def view_names(path):
    """The name of every running application, from its tree's URI.

    Discovered rather than configured: the shell derives these names itself,
    so a driver that assumed them would test its own assumption.
    """
    prefix = len(TREE_URI_PREFIX)
    return [uri[prefix:].removesuffix("/tree") for uri in list_trees(path)]


def read_tree(path, view=None):
    """One application's tree. `view` names it, and is required when more than
    one is running -- picking one would be arbitrary, and the shell refuses an
    unqualified action for the same reason."""
    trees = list_trees(path)
    if not trees:
        raise Failure(
            "no application published a semantics tree. Either semantics never "
            "came up, or nothing is being composited -- see the README on "
            "presentation before suspecting the tool under test")
    if view is not None:
        uri = f"{TREE_URI_PREFIX}{view}/tree"
        if uri not in trees:
            raise Failure(f"no tree for view {view!r}; running: {trees}")
    elif len(trees) == 1:
        uri = trees[0]
    else:
        raise Failure(
            f"{len(trees)} applications are running and no view was named: "
            f"{trees}")
    reply = rpc(path, "resources/read", {"uri": uri}, request_id=3)
    if "error" in reply:
        raise Failure(f"resources/read: {reply['error']}")
    return json.loads(reply["result"]["contents"][0]["text"])


def find(tree, label=None, role=None, identifier=None):
    """Finds a node by label and role, preferring an identifier when the
    engine reports one.

    Label and role are the primary key on purpose. Flutter 3.38.3 -- the
    oldest engine this port must run against -- never writes
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


def status_fields(path, view=None):
    """The app's status node, parsed into a dict. Values are space separated
    key=value pairs, which survives a round trip through a semantics value
    without needing the app to embed JSON in a label."""
    tree = read_tree(path, view)
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


def wait_for(path, predicate, timeout, what, view=None):
    """Polls the status node until the app reports what was expected.

    Polled rather than driven by a notification because the check is about a
    widget handler having run, which is one frame later than the dispatch the
    server acknowledged -- and a test that raced that would fail
    intermittently for a reason unrelated to what it covers.
    """
    deadline = time.time() + timeout
    last = {}
    while time.time() < deadline:
        last = status_fields(path, view)
        if predicate(last):
            return last
        time.sleep(0.05)
    raise Failure(f"timed out waiting for {what}; status was {last}")


def viewed(arguments, view):
    """Adds the view argument when one is named.

    Omitted rather than defaulted when there is a single application, because
    that is the case the shell allows to go unqualified -- sending it anyway
    would stop this driver from covering it.
    """
    if view is None:
        return arguments
    return {**arguments, "view": view}


def query(path, view=None, **selectors):
    """ui_query with any of its selectors. Returns the decoded result."""
    ok, doc = call_tool(path, "ui_query", viewed(selectors, view), request_id=7)
    if not ok:
        raise Failure(f"ui_query {selectors}: {doc}")
    return doc


def matches(doc):
    """The matched nodes from a ui_query result, in either shape it arrives in.

    A read that names a view answers about that view directly. A read that
    names none spans every application and reports each under `views`, so the
    counts and nodes sit one level down. Which shape comes back depends on how
    many applications are running rather than on what the caller asked, so a
    driver has to handle both -- reading `match_count` off the spanning shape
    finds nothing and looks exactly like an app that published no such node.

    Each returned node carries the view it came from, which is the whole point
    of the spanning shape: an agent searching for a control it has not found
    yet needs to know which application answered.
    """
    if "views" in doc:
        found = []
        for entry in doc.get("views") or []:
            for node in (entry.get("result") or {}).get("nodes", []):
                found.append({**node, "view": entry.get("view")})
        return found
    return [dict(node) for node in doc.get("nodes", [])]


def act(path, tool, arguments, what, view=None):
    """Invokes an action tool and returns its verify-after-act result.

    The response carries the node as it stands after the action, so a caller
    does not have to re-read the tree to find out what happened. Checking that
    is the point: it is what lets an agent act without a second round trip.
    """
    ok, doc = call_tool(path, tool, viewed(arguments, view), request_id=8)
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

    # Which applications are running, discovered from the shell rather than
    # configured here. With one, actions go unqualified -- that is the case the
    # shell allows and this driver has always covered. With several, every
    # action must name its view, so the checks below name the first one and the
    # multi-view checks cover the addressing itself.
    views = view_names(args.socket)
    if not views:
        raise Failure(
            "no application published a semantics tree. Either semantics never "
            "came up, or nothing is being composited -- see the README on "
            "presentation before suspecting the tool under test")
    view = views[0] if len(views) > 1 else None
    if view is not None:
        print(f"{len(views)} applications running: {', '.join(views)}")
        print(f"single-application checks run against {view!r}\n")

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
        tree = read_tree(args.socket, view)
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
        before = status_fields(args.socket, view)
        node = find(read_tree(args.socket, view), label="Save", role="button",
                    identifier="save")
        ok, doc = call_tool(args.socket, "ui_tap",
                            viewed({"node_id": node["id"]}, view), 10)
        if not ok:
            raise Failure(f"ui_tap refused: {doc}")
        after = wait_for(
            args.socket,
            lambda f: f.get("last") == "tap:save"
            and f.get("events") != before.get("events"),
            args.timeout, "the Save handler to run", view)
        del after

    # C3 -- the text actually lands in the field, which nothing else in the
    # suite covers.
    def c3():
        node = find(read_tree(args.socket, view), label="Destination",
                    identifier="destination")
        ok, doc = call_tool(args.socket, "ui_set_text",
                            viewed({"node_id": node["id"], "text": "Sarah Connor"}, view), 11)
        if not ok:
            raise Failure(f"ui_set_text refused: {doc}")
        wait_for(args.socket,
                 lambda f: f.get("text") == "Sarah Connor",
                 args.timeout, "the field to take the new text", view)

    # C4 -- in both directions: a dispatch cannot reach what the tree does not
    # describe, and a pointer tap can.
    def c4_no_node():
        tree = read_tree(args.socket, view)
        for node in tree.get("nodes", []):
            if (node.get("label") or "") == "unannotated region":
                raise Failure("the excluded region appeared in the tree; "
                              "ExcludeSemantics is not doing its job and the "
                              "contrast this fixture exists to show is gone")

    def c4_pointer_reaches():
        fields = status_fields(args.socket, view)
        geometry = fields.get("opaque", "")
        try:
            left, top, width, height = (float(v) for v in geometry.split(","))
        except ValueError as exc:
            raise Failure(f"app did not report its geometry: {geometry!r}") from exc
        before = status_fields(args.socket, view)
        ok, doc = call_tool(args.socket, "ui_tap_at",
                            viewed({"x": left + width / 2, "y": top + height / 2}, view), 12)
        if not ok:
            raise Failure(f"ui_tap_at refused: {doc}")
        wait_for(
            args.socket,
            lambda f: f.get("last") == "tap:opaque"
            and f.get("events") != before.get("events"),
            args.timeout, "the unannotated region to receive a pointer tap", view)

    # C5 -- a disabled control is refused rather than dispatched into silence.
    def c5():
        node = find(read_tree(args.socket, view), label="Locked", role="button",
                    identifier="locked")
        before = status_fields(args.socket, view)
        ok, doc = call_tool(args.socket, "ui_tap",
                            viewed({"node_id": node["id"]}, view), 13)
        if ok:
            raise Failure("ui_tap on a disabled control was accepted")
        # The refusal has to say why. A bare "refused" leaves an agent unable
        # to tell a disabled control from a missing one, and it retried.
        if "disabled" not in json.dumps(doc):
            raise Failure(f"the refusal did not say why: {doc}")
        time.sleep(0.2)
        after = status_fields(args.socket, view)
        if after.get("events") != before.get("events"):
            raise Failure("a refused tap still reached the widget")

    # C6 -- a multi-step flow driven entirely by what
    # an agent can actually see, with each step verified from the action's own
    # response rather than by re-reading the tree.
    #
    # No node ids are carried in from outside and no identifiers are used:
    # identifiers are empty on the oldest engine this port must run against,
    # so a flow that depended on them would pass here and fail on a target.
    def c6_multi_step_flow():
        # Step 1: find the control by role and the action it offers, which is
        # how an agent picks a target it was not told about.
        found = matches(query(args.socket, view, role="slider",
                              action="increase"))
        if not found:
            raise Failure("no slider offering increase; the app did not "
                          "publish one, or the selectors did not match it")
        slider = found[0]
        if slider.get("label") != "Temperature":
            raise Failure(f"found the wrong slider: {slider.get('label')!r}")

        start = numeric(slider.get("value"))
        if start is None:
            raise Failure(f"slider reports no numeric value: {slider!r}")
        app_start = numeric(status_fields(args.socket, view).get("temp"))
        if app_start is None:
            raise Failure("the app did not report its temperature")

        # Step 2: act, and read the outcome out of the action's own reply.
        first = act(args.socket, "ui_increase", {"node_id": slider["id"]},
            "raise the temperature", view)
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
            "raise it again", view)
        twice = numeric((second.get("node_after") or {}).get("value"))
        if twice is None or twice <= raised:
            raise Failure(f"second step did not advance: {raised} -> {twice}")

        # Step 4: a custom action, found and invoked by its label. These are
        # the application's own verbs -- there is no fixed set, and the label
        # is the only handle an agent has on one.
        climate = matches(query(args.socket, view,
                               custom_action="Set to Auto"))
        if not climate:
            raise Failure("no node declares the custom action 'Set to Auto'")
        node = climate[0]
        act(args.socket, "ui_custom_action",
            {"node_id": node["id"], "action": "Set to Auto"},
            "switch the climate mode", view)

        # The custom action's effect is app state rather than node state, so
        # this one step is confirmed through the status line -- which is also
        # the proof the widget's own closure ran, not merely that the dispatch
        # was accepted.
        fields = wait_for(args.socket, lambda f: f.get("mode") == "auto",
                          args.timeout, "the custom action's handler to run", view)

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
            args.timeout, "the application's tool handler to run", view)
        reported = numeric(fields.get("temp"))
        if reported is None or abs(reported - 24.5) > 0.01:
            raise Failure(
                f"the handler ran but the value did not land: {reported}")

        # A tool that runs and fails is distinct from one that does not exist.
        refused, why = call_tool(args.socket, "fixture_set_temp", {}, 22)
        if refused:
            raise Failure("a call with no celsius was accepted")
        # The message comes from the Dart handler's own exception. For a tool
        # the application declared it is the only thing that can say what went
        # wrong, since the shell knows nothing about what the tool does.
        if "celsius" not in json.dumps(why):
            raise Failure(f"the handler's reason did not reach the client: {why}")
        missing, _ = call_tool(args.socket, "fixture_nonexistent", {}, 23)
        if missing:
            raise Failure("a call to a tool that does not exist was accepted")

    # C8-C11 -- addressing, which only exists once more than one application is
    # running. Every other MCP multi-view test uses a mock host with names it
    # chose; these run against the names the shell derives for itself, which is
    # the part no unit test can reach.
    def c8_each_view_is_addressable():
        trees = list_trees(args.socket)
        if len(trees) != len(views):
            raise Failure(f"{len(views)} applications but {len(trees)} trees")
        if len(set(views)) != len(views):
            raise Failure(
                f"two applications share a name, so neither can be addressed "
                f"unambiguously: {views}")
        for name in views:
            tree = read_tree(args.socket, name)
            if find(tree, label="status", identifier="status") is None:
                raise Failure(f"view {name!r} published no status node")

    def c9_an_action_goes_to_the_view_it_names():
        other = views[1]
        before = {name: status_fields(args.socket, name) for name in views}
        node = find(read_tree(args.socket, other), label="Save", role="button",
                    identifier="save")
        ok, doc = call_tool(args.socket, "ui_tap",
                            {"node_id": node["id"], "view": other}, 30)
        if not ok:
            raise Failure(f"ui_tap naming {other!r} was refused: {doc}")
        wait_for(args.socket,
                 lambda f: f.get("last") == "tap:save"
                 and f.get("events") != before[other].get("events"),
                 args.timeout, f"the Save handler in {other!r} to run", other)
        # The other applications must be untouched. Both run the same bundle
        # and so have a node of the same id -- which is exactly the collision
        # that makes an unaddressed dispatch land in the wrong application.
        for name in views:
            if name == other:
                continue
            if status_fields(args.socket, name).get("events") != \
                    before[name].get("events"):
                raise Failure(
                    f"tapping {other!r} also reached {name!r}; the action was "
                    f"delivered by node id rather than by the view named")

    def c10_an_unqualified_action_is_refused():
        before = {name: status_fields(args.socket, name) for name in views}
        node = find(read_tree(args.socket, views[0]), label="Save",
                    role="button", identifier="save")
        ok, doc = call_tool(args.socket, "ui_tap", {"node_id": node["id"]}, 31)
        if ok:
            raise Failure(
                "an action naming no view was accepted while several "
                "applications were running; one of them received it")
        # The refusal has to name what is missing, or an agent cannot learn
        # that the argument exists.
        if "view" not in json.dumps(doc):
            raise Failure(f"the refusal did not say a view was needed: {doc}")
        time.sleep(0.2)
        for name in views:
            if status_fields(args.socket, name).get("events") != \
                    before[name].get("events"):
                raise Failure(f"the refused action still reached {name!r}")

    def c11_an_unqualified_read_spans_every_view():
        # A read is the opposite case from an action: an agent hunting for a
        # control does not yet know which application owns it, so searching all
        # of them and saying where each hit came from is the useful answer.
        found = matches(query(args.socket, None, label="Save"))
        if len(found) < len(views):
            raise Failure(
                f"an unqualified read found {len(found)} matches across "
                f"{len(views)} applications, each of which has a Save: "
                f"{found}")
        attributed = {node.get("view") for node in found}
        missing = [name for name in views if name not in attributed]
        if missing:
            raise Failure(
                f"the result does not say which view these came from, so an "
                f"agent cannot act on them: {missing} absent from {attributed}")

    check("C1", "the tree describes the app", c1)
    check("C2", "ui_tap invokes the widget's own handler", c2)
    check("C3", "ui_set_text reaches the field", c3)
    check("C4a", "the unannotated region is absent from the tree", c4_no_node)
    check("C4b", "ui_tap_at reaches it anyway", c4_pointer_reaches)
    check("C5", "a disabled control is refused before dispatch", c5)
    check("C6", "a multi-step flow by role, label and custom-action name",
          c6_multi_step_flow)
    check("C7", "a tool the application declared, with its schema",
          c7_app_declared_tool)
    if len(views) > 1:
        check("C8", "every application has its own addressable tree",
              c8_each_view_is_addressable)
        check("C9", "an action goes only to the view it names",
              c9_an_action_goes_to_the_view_it_names)
        check("C10", "an action naming no view is refused",
              c10_an_unqualified_action_is_refused)
        check("C11", "a read naming no view spans every one",
              c11_an_unqualified_read_spans_every_view)

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
