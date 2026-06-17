// scroll_bench — a deterministic, self-driving scrollable list benchmark.
//
// Renders a long list of "information" rows (icon + title + two-line detail +
// trailing value + divider) and, by default, scrolls it continuously at a
// fixed velocity so frame cadence can be profiled without any input injection.
//
// Drive it with the homescreen software backend and IVI_SW_PROFILE to read
// fps over the drm-cxx DrmDumbSink present path:
//   IVI_SW_PROFILE=1 homescreen -b <bundle> --window-type=NORMAL -f
//
// Tunables (compile-time --dart-define, all optional):
//   ITEMS      number of rows                       (default 2000)
//   VELOCITY   auto-scroll speed, logical px/s      (default 600)
//   AUTOSCROLL "false" disables auto-scroll (manual)(default on)
import 'package:flutter/material.dart';

const int kItems = int.fromEnvironment('ITEMS', defaultValue: 2000);
const int kVelocity = int.fromEnvironment('VELOCITY', defaultValue: 600);
const bool kAutoScroll =
    bool.fromEnvironment('AUTOSCROLL', defaultValue: true);

void main() => runApp(const ScrollBenchApp());

class ScrollBenchApp extends StatelessWidget {
  const ScrollBenchApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'scroll_bench',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.indigo),
        useMaterial3: true,
      ),
      home: const InfoListPage(),
    );
  }
}

// Deterministic per-row content derived purely from the index, so every run
// renders identical pixels and the benchmark is reproducible.
class _Row {
  const _Row(this.index);
  final int index;

  static const _categories = <String>[
    'Powertrain', 'Chassis', 'Body', 'Network', 'Infotainment',
    'ADAS', 'Climate', 'Battery', 'Telematics', 'Diagnostics',
  ];
  static const _icons = <IconData>[
    Icons.speed, Icons.thermostat, Icons.battery_charging_full,
    Icons.settings_input_antenna, Icons.memory, Icons.directions_car,
    Icons.ac_unit, Icons.bolt, Icons.satellite_alt, Icons.build_circle,
  ];
  static const _palette = <Color>[
    Color(0xFF5C6BC0), Color(0xFF26A69A), Color(0xFF66BB6A),
    Color(0xFFFFA726), Color(0xFFEF5350), Color(0xFFAB47BC),
    Color(0xFF42A5F5), Color(0xFFEC407A), Color(0xFF8D6E63),
    Color(0xFF78909C),
  ];

  int get _cat => index % _categories.length;
  String get category => _categories[_cat];
  IconData get icon => _icons[_cat];
  Color get color => _palette[_cat];

  String get title => '$category signal #${index.toString().padLeft(4, '0')}';
  String get subtitle =>
      'ECU 0x${(0x700 + (index % 64)).toRadixString(16).toUpperCase()} · '
      'PID ${(index * 7) % 256} · last seen ${index % 60}s ago';
  String get value {
    final v = ((index * 37) % 1000) / 10.0; // 0.0 .. 99.9, stable per index
    return v.toStringAsFixed(1);
  }

  String get unit {
    switch (_cat) {
      case 0:
        return 'km/h';
      case 1:
        return '°C';
      case 7:
        return 'V';
      default:
        return '%';
    }
  }
}

class InfoListPage extends StatefulWidget {
  const InfoListPage({super.key});
  @override
  State<InfoListPage> createState() => _InfoListPageState();
}

class _InfoListPageState extends State<InfoListPage> {
  final ScrollController _controller = ScrollController();
  bool _running = kAutoScroll;

  @override
  void initState() {
    super.initState();
    if (kAutoScroll) {
      WidgetsBinding.instance.addPostFrameCallback((_) => _loop());
    }
  }

  // Continuous constant-velocity scroll: animate to the bottom, snap back to
  // the top, repeat. A linear curve keeps px/s steady so cadence is uniform.
  void _loop() {
    if (!mounted || !_running || !_controller.hasClients) return;
    final max = _controller.position.maxScrollExtent;
    final ms = (max / kVelocity * 1000).round().clamp(1000, 600000);
    _controller
        .animateTo(max,
            duration: Duration(milliseconds: ms), curve: Curves.linear)
        .then((_) {
      if (!mounted || !_running || !_controller.hasClients) return;
      _controller.jumpTo(0);
      _loop();
    });
  }

  void _toggle() {
    setState(() => _running = !_running);
    if (_running) {
      _loop();
    } else {
      _controller.jumpTo(_controller.offset); // stop the animation
    }
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Vehicle signals · $kItems rows'),
        actions: [
          IconButton(
            icon: Icon(_running ? Icons.pause : Icons.play_arrow),
            onPressed: _toggle,
          ),
        ],
      ),
      body: ListView.builder(
        controller: _controller,
        itemCount: kItems,
        itemExtent: 76.0, // fixed extent: cheap layout, realistic info row
        itemBuilder: (context, i) => _InfoTile(row: _Row(i)),
      ),
    );
  }
}

class _InfoTile extends StatelessWidget {
  const _InfoTile({required this.row});
  final _Row row;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return DecoratedBox(
      decoration: const BoxDecoration(
        border: Border(bottom: BorderSide(color: Color(0x14000000))),
      ),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
        child: Row(
          children: [
            CircleAvatar(
              radius: 24,
              backgroundColor: row.color.withValues(alpha: 0.18),
              child: Icon(row.icon, color: row.color),
            ),
            const SizedBox(width: 16),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  Text(row.title,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: theme.textTheme.titleMedium),
                  const SizedBox(height: 2),
                  Text(row.subtitle,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: theme.textTheme.bodySmall
                          ?.copyWith(color: Colors.black54)),
                ],
              ),
            ),
            const SizedBox(width: 12),
            Column(
              crossAxisAlignment: CrossAxisAlignment.end,
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                Text(row.value,
                    style: theme.textTheme.titleLarge?.copyWith(
                        color: row.color, fontWeight: FontWeight.w600)),
                Text(row.unit,
                    style: theme.textTheme.labelSmall
                        ?.copyWith(color: Colors.black45)),
              ],
            ),
          ],
        ),
      ),
    );
  }
}
