// A cockpit demo for driving ivi-homescreen from an LLM.
//
// It offers an agent both routes into the UI, side by side, so the difference
// is visible:
//
//   * the generic verbs the shell derives from the semantics tree, which cost
//     the application nothing beyond being accessible -- an agent finds the
//     fan buttons by label and taps them
//   * tools this application declares for itself, with schemas, for the
//     operations that have no generic form: set the temperature to 21.5
//     rather than pressing "warmer" an unknown number of times
//
// Everything an agent does shows up in the activity log on screen, so a person
// watching can see what happened and in which order.

import 'package:flutter/material.dart';
import 'package:ihs_mcp_app_tools/ihs_mcp_app_tools.dart';

void main() {
  runApp(const CockpitDemo());
}

class CockpitDemo extends StatelessWidget {
  const CockpitDemo({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Cockpit',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF00629B),
          brightness: Brightness.dark,
        ),
        useMaterial3: true,
      ),
      home: const CockpitPage(),
    );
  }
}

class CockpitPage extends StatefulWidget {
  const CockpitPage({super.key});

  @override
  State<CockpitPage> createState() => _CockpitPageState();
}

class _CockpitPageState extends State<CockpitPage> {
  static const List<String> _zones = <String>['driver', 'passenger', 'rear'];

  final Map<String, double> _temperature = <String, double>{
    'driver': 21.0,
    'passenger': 21.0,
    'rear': 19.0,
  };
  int _fan = 2;
  bool _ac = true;
  String _zone = 'driver';

  bool _playing = false;
  final List<String> _tracks = <String>[
    'Kind of Blue',
    'A Love Supreme',
    'Time Out',
  ];
  int _track = 0;
  double _volume = 4;

  String _destination = 'none';
  final TextEditingController _destinationField =
      TextEditingController(text: 'none');

  final List<String> _log = <String>[];
  McpAppTools? _tools;
  String _toolStatus = 'registering';

  @override
  void initState() {
    super.initState();
    _registerTools();
  }

  @override
  void dispose() {
    // Withdrawn before the state goes. A tool left advertised after its
    // handler is gone fails only after the call times out, which reads to an
    // agent as the whole surface being broken.
    _tools?.unregister();
    _destinationField.dispose();
    super.dispose();
  }

  void _note(String entry) {
    setState(() {
      _log.insert(0, entry);
      if (_log.length > 12) {
        _log.removeLast();
      }
    });
  }

  // ---------------------------------------------------------------------
  // Tools this application declares
  // ---------------------------------------------------------------------

  void _registerTools() {
    try {
      _tools = McpAppTools.register(
        prefix: 'cockpit_',
        tools: <McpTool>[
          McpTool(
            name: 'status',
            description:
                'Read the whole cockpit state: climate, media and navigation.',
            inputSchema: <String, Object?>{
              'type': 'object',
              'properties': <String, Object?>{},
            },
            handler: (Map<String, Object?> arguments) => _status(),
          ),
          McpTool(
            name: 'set_temperature',
            description: 'Set one zone to an exact temperature in celsius.',
            inputSchema: <String, Object?>{
              'type': 'object',
              'properties': <String, Object?>{
                'zone': <String, Object?>{
                  'type': 'string',
                  'enum': _zones,
                  'description': 'Which seat the temperature applies to.',
                },
                'celsius': <String, Object?>{
                  'type': 'number',
                  'minimum': 16,
                  'maximum': 28,
                },
              },
              'required': <String>['zone', 'celsius'],
            },
            handler: (Map<String, Object?> arguments) async {
              final String zone = _requireZone(arguments['zone']);
              final num celsius = _requireNum(arguments['celsius'], 'celsius');
              if (celsius < 16 || celsius > 28) {
                throw ArgumentError(
                    'celsius must be between 16 and 28, got $celsius');
              }
              setState(() => _temperature[zone] = celsius.toDouble());
              _note('set_temperature($zone, $celsius)');
              return <String, Object?>{'zone': zone, 'celsius': celsius};
            },
          ),
          McpTool(
            name: 'set_fan',
            description: 'Set the fan speed, 0 (off) to 5.',
            inputSchema: <String, Object?>{
              'type': 'object',
              'properties': <String, Object?>{
                'speed': <String, Object?>{
                  'type': 'integer',
                  'minimum': 0,
                  'maximum': 5,
                },
              },
              'required': <String>['speed'],
            },
            handler: (Map<String, Object?> arguments) async {
              final num speed = _requireNum(arguments['speed'], 'speed');
              if (speed < 0 || speed > 5) {
                throw ArgumentError('speed must be 0 to 5, got $speed');
              }
              setState(() => _fan = speed.round());
              _note('set_fan($speed)');
              return <String, Object?>{'speed': _fan};
            },
          ),
          McpTool(
            name: 'media',
            description: 'Control playback. next and previous also start '
                'playback if it was paused.',
            inputSchema: <String, Object?>{
              'type': 'object',
              'properties': <String, Object?>{
                'action': <String, Object?>{
                  'type': 'string',
                  'enum': <String>['play', 'pause', 'next', 'previous'],
                },
              },
              'required': <String>['action'],
            },
            handler: (Map<String, Object?> arguments) async {
              final String action = '${arguments['action']}';
              switch (action) {
                case 'play':
                  setState(() => _playing = true);
                case 'pause':
                  setState(() => _playing = false);
                // Skipping starts playback, as a head unit does. Advancing to
                // a track and leaving it silent is the literal reading of
                // "next" and not what anyone means by it.
                case 'next':
                  setState(() {
                    _track = (_track + 1) % _tracks.length;
                    _playing = true;
                  });
                case 'previous':
                  setState(() {
                    _track = (_track - 1 + _tracks.length) % _tracks.length;
                    _playing = true;
                  });
                default:
                  throw ArgumentError('unknown action: $action');
              }
              _note('media($action)');
              return <String, Object?>{
                'playing': _playing,
                'track': _tracks[_track],
              };
            },
          ),
          McpTool(
            name: 'set_destination',
            description: 'Set the navigation destination.',
            inputSchema: <String, Object?>{
              'type': 'object',
              'properties': <String, Object?>{
                'place': <String, Object?>{'type': 'string'},
              },
              'required': <String>['place'],
            },
            handler: (Map<String, Object?> arguments) async {
              final String place = '${arguments['place'] ?? ''}'.trim();
              if (place.isEmpty) {
                throw ArgumentError('place must not be empty');
              }
              setState(() {
                _destination = place;
                // Kept in step with the field: setting one and not the other
                // leaves the UI showing something the state does not agree
                // with, which is the failure a person notices last.
                _destinationField.value = TextEditingValue(
                  text: place,
                  selection: TextSelection.collapsed(offset: place.length),
                );
              });
              _note('set_destination($place)');
              return <String, Object?>{'destination': place};
            },
          ),
        ],
      );
      setState(() => _toolStatus = 'registered under cockpit_');
    } on Object catch (error) {
      // Reported on screen rather than thrown. The shell may be built without
      // the MCP surface, and the demo is still worth looking at -- the
      // semantics verbs work regardless, because they need nothing from here.
      setState(() => _toolStatus = 'unavailable: $error');
    }
  }

  String _requireZone(Object? value) {
    final String zone = '${value ?? ''}';
    if (!_zones.contains(zone)) {
      throw ArgumentError('zone must be one of ${_zones.join(", ")}, '
          'got "$zone"');
    }
    return zone;
  }

  num _requireNum(Object? value, String name) {
    if (value is num) {
      return value;
    }
    throw ArgumentError('$name must be a number, got ${value.runtimeType}');
  }

  Map<String, Object?> _status() => <String, Object?>{
        'climate': <String, Object?>{
          'temperature': _temperature,
          'fan': _fan,
          'ac': _ac,
          'selected_zone': _zone,
        },
        'media': <String, Object?>{
          'playing': _playing,
          'track': _tracks[_track],
          'volume': _volume.round(),
        },
        'navigation': <String, Object?>{'destination': _destination},
      };

  // ---------------------------------------------------------------------
  // UI. Every control is annotated so the generic verbs reach it too.
  // ---------------------------------------------------------------------

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Cockpit'),
        actions: <Widget>[
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 16),
            child: Center(
              child: Text(_toolStatus,
                  style: Theme.of(context).textTheme.bodySmall),
            ),
          ),
        ],
      ),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Expanded(flex: 3, child: _panels()),
            const SizedBox(width: 24),
            Expanded(flex: 2, child: _activityLog(context)),
          ],
        ),
      ),
    );
  }

  Widget _panels() {
    return ListView(
      children: <Widget>[
        _card('Climate', <Widget>[
          Row(
            children: <Widget>[
              for (final String zone in _zones)
                Padding(
                  padding: const EdgeInsets.only(right: 8),
                  // Merged so the label and the tap land on one node. A bare
                  // Semantics wrapper would put the label on a parent and
                  // leave the actionable node unnamed, which an agent cannot
                  // address.
                  child: MergeSemantics(
                    child: Semantics(
                      identifier: 'zone_$zone',
                      button: true,
                      selected: _zone == zone,
                      child: ChoiceChip(
                        label: Text(zone),
                        selected: _zone == zone,
                        onSelected: (_) {
                          setState(() => _zone = zone);
                          _note('selected zone $zone');
                        },
                      ),
                    ),
                  ),
                ),
            ],
          ),
          const SizedBox(height: 8),
          MergeSemantics(
            child: Semantics(
              identifier: 'temperature',
              label: 'Temperature for $_zone',
              child: Slider(
                value: _temperature[_zone]!,
                min: 16,
                max: 28,
                divisions: 24,
                label: '${_temperature[_zone]!.toStringAsFixed(1)} C',
                onChanged: (double value) {
                  setState(() => _temperature[_zone] = value);
                },
                onChangeEnd: (double value) =>
                    _note('temperature $_zone -> ${value.toStringAsFixed(1)}'),
              ),
            ),
          ),
          Text('${_temperature[_zone]!.toStringAsFixed(1)} C in $_zone'),
          const SizedBox(height: 8),
          Row(
            children: <Widget>[
              const Text('Fan'),
              const SizedBox(width: 12),
              MergeSemantics(
                child: Semantics(
                  identifier: 'fan_down',
                  child: OutlinedButton(
                    onPressed: _fan > 0
                        ? () {
                            setState(() => _fan--);
                            _note('fan -> $_fan');
                          }
                        : null,
                    child: const Text('Fan slower'),
                  ),
                ),
              ),
              const SizedBox(width: 8),
              Text('$_fan'),
              const SizedBox(width: 8),
              MergeSemantics(
                child: Semantics(
                  identifier: 'fan_up',
                  child: OutlinedButton(
                    onPressed: _fan < 5
                        ? () {
                            setState(() => _fan++);
                            _note('fan -> $_fan');
                          }
                        : null,
                    child: const Text('Fan faster'),
                  ),
                ),
              ),
              const SizedBox(width: 16),
              MergeSemantics(
                child: Semantics(
                  identifier: 'ac',
                  child: FilterChip(
                    label: const Text('A/C'),
                    selected: _ac,
                    onSelected: (bool value) {
                      setState(() => _ac = value);
                      _note('a/c ${value ? "on" : "off"}');
                    },
                  ),
                ),
              ),
            ],
          ),
        ]),
        _card('Media', <Widget>[
          Text(_tracks[_track],
              style: Theme.of(context).textTheme.titleMedium),
          const SizedBox(height: 8),
          Row(
            children: <Widget>[
              MergeSemantics(
                child: Semantics(
                  identifier: 'play_pause',
                  child: FilledButton(
                    onPressed: () {
                      setState(() => _playing = !_playing);
                      _note(_playing ? 'play' : 'pause');
                    },
                    child: Text(_playing ? 'Pause' : 'Play'),
                  ),
                ),
              ),
              const SizedBox(width: 8),
              MergeSemantics(
                child: Semantics(
                  identifier: 'next_track',
                  child: OutlinedButton(
                    onPressed: () {
                      setState(() {
                        _track = (_track + 1) % _tracks.length;
                        _playing = true;
                      });
                      _note('next track');
                    },
                    child: const Text('Next track'),
                  ),
                ),
              ),
            ],
          ),
          const SizedBox(height: 8),
          MergeSemantics(
            child: Semantics(
              identifier: 'volume',
              label: 'Volume',
              child: Slider(
                value: _volume,
                max: 10,
                divisions: 10,
                onChanged: (double value) => setState(() => _volume = value),
                onChangeEnd: (double value) =>
                    _note('volume -> ${value.round()}'),
              ),
            ),
          ),
        ]),
        _card('Navigation', <Widget>[
          MergeSemantics(
            child: Semantics(
              // No label here: the field's own labelText already provides one,
              // and MergeSemantics concatenates the two into
              // "Destination\nDestination" -- which no exact match finds and
              // which reads as two controls to anything summarising the tree.
              identifier: 'destination',
              child: TextField(
                decoration: const InputDecoration(
                  labelText: 'Destination',
                  border: OutlineInputBorder(),
                ),
                // Held rather than built per frame. A controller constructed in
                // build() is replaced on the next rebuild, so text an agent set
                // through ui_set_text would be reverted by the following frame
                // -- the write reaches the field and then quietly disappears.
                controller: _destinationField,
                onChanged: (String value) => _destination = value,
                onSubmitted: (String value) {
                  setState(() => _destination = value);
                  _note('destination -> $value');
                },
              ),
            ),
          ),
        ]),
      ],
    );
  }

  Widget _card(String title, List<Widget> children) {
    return Card(
      margin: const EdgeInsets.only(bottom: 16),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Text(title, style: Theme.of(context).textTheme.titleLarge),
            const SizedBox(height: 12),
            ...children,
          ],
        ),
      ),
    );
  }

  Widget _activityLog(BuildContext context) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Text('Activity', style: Theme.of(context).textTheme.titleLarge),
            const SizedBox(height: 12),
            // Excluded from semantics: it is here for the person watching, and
            // leaving it in the tree would fill an agent's view of the UI with
            // a transcript of its own actions.
            Expanded(
              child: ExcludeSemantics(
                child: _log.isEmpty
                    ? const Text('nothing yet')
                    : ListView(
                        children: <Widget>[
                          for (final String entry in _log)
                            Padding(
                              padding: const EdgeInsets.only(bottom: 6),
                              child: Text(entry),
                            ),
                        ],
                      ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
