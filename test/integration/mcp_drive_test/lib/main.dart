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
// CustomSemanticsAction is not exported by material.dart.
import 'package:flutter/semantics.dart';
import 'package:ihs_mcp_app_tools/ihs_mcp_app_tools.dart';

// The shell passes --ihs-view=<name> so an instance can tell which view it is.
// Held here because the tool registration needs it and initState has no access
// to the entrypoint arguments.
String? _viewName;

void main(List<String> args) {
  _viewName = McpAppTools.viewFromArgs(args);
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

  // The multi-step flow's state. A slider carries a
  // value the framework reports on the node itself, so a check can verify from
  // the action's own node_after rather than reading the status line back --
  // which is the difference verify-after-act is meant to make.
  double _temperature = 21.0;
  String _mode = 'manual';

  final TextEditingController _destination =
      TextEditingController(text: 'unset');

  // Reported so the driver can aim ui_tap_at without hard-coding a layout.
  // A fixture that encodes its own geometry breaks the first time a font or a
  // screen size changes; asking the app where the target is does not.
  final GlobalKey _opaqueKey = GlobalKey();
  Rect? _opaqueRect;

  void _setMode(String mode) {
    setState(() {
      _mode = mode;
      _last = 'mode:$mode';
      _events++;
    });
  }

  void _record(String what) {
    setState(() {
      _last = what;
      _events++;
    });
  }

  McpAppTools? _appTools;

  // A typed tool, which is the thing the semantics verbs cannot express: an
  // agent names the zone and the temperature rather than working out how many
  // times to press a control it found by role.
  void _registerAppTools() {
    try {
      _appTools = McpAppTools.register(
        prefix: 'fixture_',
        // Only breaks a tie: with one instance this changes nothing, and with
        // two the second is advertised under its view instead of losing its
        // tools entirely.
        view: _viewName,
        tools: <McpTool>[
          McpTool(
            name: 'set_temp',
            description: 'Set the cabin temperature, in celsius.',
            inputSchema: <String, Object?>{
              'type': 'object',
              'properties': <String, Object?>{
                'celsius': <String, Object?>{
                  'type': 'number',
                  'minimum': 16,
                  'maximum': 28,
                },
              },
              'required': <String>['celsius'],
            },
            handler: (Map<String, Object?> arguments) async {
              final num? celsius = arguments['celsius'] as num?;
              if (celsius == null) {
                // Thrown rather than returned: this is the tool running and
                // failing, which a client is told apart from the tool being
                // absent.
                throw ArgumentError('celsius is required');
              }
              setState(() {
                _temperature = celsius.toDouble().clamp(16.0, 28.0);
                _last = 'tool:set_temp';
                _events++;
              });
              return <String, Object?>{'celsius': _temperature};
            },
          ),
        ],
      );
    } on Object catch (error) {
      // Reported through the status line rather than thrown: the shell may be
      // built without the MCP surface, and the rest of the fixture still has
      // something to say in that case.
      setState(() => _last = 'tools:unavailable:$error');
    }
  }

  @override
  void initState() {
    super.initState();
    _registerAppTools();
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
    // Withdrawn before the state goes: a stale tool an agent can still call is
    // worse than none, because it fails only after the timeout.
    _appTools?.unregister();
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
    return 'last=$_last; events=$_events; text=${_destination.text}; '
        'temp=${_temperature.toStringAsFixed(1)}; mode=$_mode; $geometry';
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

              // A slider, for the multi-step flow. Chosen because the
              // framework puts increase/decrease on the node and reports the
              // current setting as the node's own value -- so a driver can
              // verify from node_after directly, without the status line.
              //
              // Wrapped and merged for the same reason as everything else
              // here: a bare Semantics(label:) would sit above the slider and
              // leave the actionable node unlabelled.
              MergeSemantics(
                child: Semantics(
                  identifier: 'temperature',
                  label: 'Temperature',
                  child: Slider(
                    value: _temperature,
                    min: 16,
                    max: 28,
                    divisions: 24,
                    onChanged: (double value) {
                      setState(() {
                        _temperature = value;
                        _last = 'temp';
                        _events++;
                      });
                    },
                  ),
                ),
              ),
              const SizedBox(height: 12),

              // Custom actions: the application's own verbs, which have no
              // fixed set and are named only by their labels. An agent finds
              // these through the tree and invokes them by name, since
              // identifiers are empty at the deployment floor.
              MergeSemantics(
                child: Semantics(
                  identifier: 'climate',
                  label: 'Climate',
                  customSemanticsActions: <CustomSemanticsAction, VoidCallback>{
                    const CustomSemanticsAction(label: 'Set to Auto'):
                        () => _setMode('auto'),
                    const CustomSemanticsAction(label: 'Set to Manual'):
                        () => _setMode('manual'),
                  },
                  child: Container(
                    padding: const EdgeInsets.all(12),
                    color: const Color(0xFFE0E0E0),
                    child: const ExcludeSemantics(child: Text('Climate')),
                  ),
                ),
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
