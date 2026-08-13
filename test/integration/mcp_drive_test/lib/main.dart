// Integration test app for driving ivi-homescreen over MCP.
//
// Everything else in the MCP suite runs against a mock host: those tests show
// a request reaching the shell, not that Flutter did anything with it. This
// app is the other half — a real engine, real widgets, real hit-testing — so
// the claims that only hold end to end can be checked:
//
//   * a semantics dispatch invokes the handler the widget actually registered,
//     rather than merely arriving at the funnel
//   * ui_set_text replaces a TextField's contents (plan R-9's exit criterion)
//   * ui_tap_at reaches a target the semantics tree does not describe, and
//     ui_tap cannot — the DR-7 contrast, demonstrable rather than argued
//   * a disabled control is refused before the framework silently drops it
//
// The app reports through MCP rather than stdout. Every handler writes into a
// status line that is itself a semantics node, so the driver reads results
// back over the same surface it used to act. That makes the read path the
// assertion channel for the write path: if either direction is broken the
// checks cannot pass by accident.

import 'package:flutter/material.dart';

void main() {
  runApp(const McpDriveTestApp());
}

class McpDriveTestApp extends StatelessWidget {
  const McpDriveTestApp({super.key});

  @override
  Widget build(BuildContext context) {
    return const MaterialApp(
      title: 'mcp_drive_test',
      debugShowCheckedModeBanner: false,
      home: DriveTestPage(),
    );
  }
}

class DriveTestPage extends StatefulWidget {
  const DriveTestPage({super.key});

  @override
  State<DriveTestPage> createState() => _DriveTestPageState();
}

class _DriveTestPageState extends State<DriveTestPage> {
  // What the driver reads back. `last` is the most recent handler to fire and
  // `events` counts them, so a check can tell "the handler ran again" from
  // "the handler ran once and the value happens to match".
  String _last = 'none';
  int _events = 0;

  final TextEditingController _destination =
      TextEditingController(text: 'unset');

  // Reported so the driver can aim ui_tap_at without hard-coding a layout.
  // A fixture that encodes its own geometry breaks the first time a font or a
  // screen size changes; asking the app where the target is does not.
  final GlobalKey _opaqueKey = GlobalKey();
  Rect? _opaqueRect;

  void _record(String what) {
    setState(() {
      _last = what;
      _events++;
    });
  }

  @override
  void initState() {
    super.initState();
    // After the first layout, publish where the unannotated region ended up.
    WidgetsBinding.instance.addPostFrameCallback((_) => _publishOpaqueRect());
  }

  void _publishOpaqueRect() {
    final RenderObject? box = _opaqueKey.currentContext?.findRenderObject();
    if (box is! RenderBox || !box.hasSize) {
      return;
    }
    final Offset origin = box.localToGlobal(Offset.zero);
    setState(() {
      _opaqueRect = origin & box.size;
    });
  }

  @override
  void dispose() {
    _destination.dispose();
    super.dispose();
  }

  String get _status {
    final Rect? r = _opaqueRect;
    final String geometry = r == null
        ? 'opaque=unknown'
        : 'opaque=${r.left.toStringAsFixed(1)},${r.top.toStringAsFixed(1)},'
            '${r.width.toStringAsFixed(1)},${r.height.toStringAsFixed(1)}';
    // Semicolon separated, not space: one of the fields is text a driver
    // chose, and the realistic case for set_text has a space in it. A
    // separator the payload can contain makes the fixture pass only for
    // inputs that avoid it, which is the wrong way round.
    return 'last=$_last; events=$_events; text=${_destination.text}; $geometry';
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: SafeArea(
        child: Padding(
          padding: const EdgeInsets.all(16),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              // The report channel: one node whose value carries the whole
              // result, so a single read gets everything a check needs.
              //
              // ExcludeSemantics on the child matters. Without it the Text
              // contributes its own label, the two merge, and the node ends up
              // labelled "status\nlast=..." -- which no exact match finds.
              Semantics(
                identifier: 'status',
                label: 'status',
                value: _status,
                child: ExcludeSemantics(child: Text(_status)),
              ),
              const SizedBox(height: 12),

              // MergeSemantics on every control below, and this is the part
              // worth copying into a real app. Semantics(identifier:) used as
              // a wrapper produces a *parent* node carrying the identifier and
              // no actions, with the actionable node underneath it -- so an
              // agent that resolves the identifier and dispatches to it is
              // refused, because that node offers nothing. Merging puts the
              // identifier and the action on one node.
              MergeSemantics(
                child: Semantics(
                  identifier: 'save',
                  child: ElevatedButton(
                    onPressed: () => _record('tap:save'),
                    child: const Text('Save'),
                  ),
                ),
              ),
              const SizedBox(height: 8),

              // Disabled: the framework drops an action on it silently, so the
              // provider must refuse before dispatching.
              MergeSemantics(
                child: Semantics(
                  identifier: 'locked',
                  child: const ElevatedButton(
                    onPressed: null,
                    child: Text('Locked'),
                  ),
                ),
              ),
              const SizedBox(height: 8),

              SizedBox(
                width: 320,
                child: MergeSemantics(
                  child: Semantics(
                    identifier: 'destination',
                    child: TextField(
                      controller: _destination,
                      decoration: const InputDecoration(
                        labelText: 'Destination',
                      ),
                      onChanged: (String value) => _record('text:$value'),
                    ),
                  ),
                ),
              ),
              const SizedBox(height: 12),

              // The DR-7 case. ExcludeSemantics removes it from the tree
              // entirely, so no node describes it and ui_tap has nothing to
              // address -- the custom-canvas or platform-view target the
              // pointer fallback exists for. It still hit-tests, so ui_tap_at
              // at the rect the app reports reaches it.
              ExcludeSemantics(
                child: GestureDetector(
                  key: _opaqueKey,
                  behavior: HitTestBehavior.opaque,
                  onTap: () => _record('tap:opaque'),
                  child: Container(
                    width: 240,
                    height: 80,
                    color: Colors.blueGrey,
                    alignment: Alignment.center,
                    child: const Text(
                      'unannotated region',
                      style: TextStyle(color: Colors.white),
                    ),
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
