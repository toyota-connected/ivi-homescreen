// Integration test app for the modern HardwareKeyboard pipeline.
//
// Exercises every public surface of services.HardwareKeyboard
// (https://api.flutter.dev/flutter/services/HardwareKeyboard-class.html)
// without touching any legacy keyboard plumbing:
//
//   * No TextField / TextInputPlugin (flutter/textinput channel).
//   * No RawKeyboard / RawKeyEvent (flutter/keyevent channel).
//   * No Shortcuts / Actions / Focus traversal that would route through the
//     text-input system.
//
// The embedder's only contract under test is FlutterEngineSendKeyEvent
// landing as KeyEvent objects on HardwareKeyboard.instance.
//
// Three panels:
//
//   1. Live key-event log
//      Every KeyDownEvent / KeyUpEvent / KeyRepeatEvent with physicalKey,
//      logicalKey, character, timeStamp, deviceType, and synthesized flag.
//
//   2. Live state snapshot
//      HardwareKeyboard.instance.{physicalKeysPressed, logicalKeysPressed,
//      lockModesEnabled, isShiftPressed, isControlPressed, isAltPressed,
//      isMetaPressed} — re-rendered on every event.
//
//   3. Coverage checklist
//      Every mode the test cares about. Items flip green the first time
//      they are observed. "Reset" clears, "Self-check" prints a JSON
//      summary line that CI can scrape.

import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

void main() {
  FlutterError.onError = (FlutterErrorDetails details) {
    FlutterError.dumpErrorToConsole(details);
    if (details.stack != null) {
      debugPrint('STACK:\n${details.stack}');
    }
  };
  runApp(const HardwareKeyboardTestApp());
}

class HardwareKeyboardTestApp extends StatelessWidget {
  const HardwareKeyboardTestApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'ivi-homescreen HardwareKeyboard test',
      theme: ThemeData(
        colorSchemeSeed: Colors.teal,
        brightness: Brightness.dark,
        useMaterial3: true,
      ),
      home: const _Home(),
    );
  }
}

// ---------------------------------------------------------------------------
// Coverage tracking
// ---------------------------------------------------------------------------

/// Each value names something the integration test wants to observe via
/// HardwareKeyboard. Strictly hardware-keyboard surface area — nothing here
/// requires the legacy keyboard plumbing.
enum Mode {
  // Event types
  keyDown,
  keyUp,
  keyRepeat,

  // Synthesized vs. real
  realEvent,
  synthesizedEvent,

  // Modifier convenience getters
  modShift,
  modCtrl,
  modAlt,
  modMeta,

  // Lock modes
  lockCaps,
  lockNum,
  lockScroll,

  // Modifier keys observed as logical keys in their own KeyEvent
  modifierKeyEvent,

  // Pressed-set tracking
  physicalKeyTracked,
  logicalKeyTracked,
  multiKeyChord,

  // Character delivery
  characterReceived,
  nonAsciiCharacter,

  // Logical key categories
  printableLetter,
  printableDigit,
  arrowKey,
  navKey,
  functionKey,
  tabKey,
  escapeKey,
  enterKey,
  spaceKey,
  backspaceKey,

  // Programmatic API
  syncKeyboardState,
  clearStateCalled,
}

extension on Mode {
  String get label => switch (this) {
        Mode.keyDown => 'KeyDownEvent received',
        Mode.keyUp => 'KeyUpEvent received',
        Mode.keyRepeat => 'KeyRepeatEvent received',
        Mode.realEvent => 'Non-synthesized event received',
        Mode.synthesizedEvent => 'Synthesized event received',
        Mode.modShift => 'isShiftPressed observed true',
        Mode.modCtrl => 'isControlPressed observed true',
        Mode.modAlt => 'isAltPressed observed true',
        Mode.modMeta => 'isMetaPressed observed true',
        Mode.lockCaps => 'lockModesEnabled contains capsLock',
        Mode.lockNum => 'lockModesEnabled contains numLock',
        Mode.lockScroll => 'lockModesEnabled contains scrollLock',
        Mode.modifierKeyEvent =>
          'KeyEvent whose logicalKey is itself a modifier',
        Mode.physicalKeyTracked => 'physicalKeysPressed updated',
        Mode.logicalKeyTracked => 'logicalKeysPressed updated',
        Mode.multiKeyChord => 'Two or more physical keys held at once',
        Mode.characterReceived => 'KeyEvent.character delivered',
        Mode.nonAsciiCharacter => 'Non-ASCII character delivered',
        Mode.printableLetter => 'Printable ASCII letter (a-z / A-Z)',
        Mode.printableDigit => 'Digit row (0-9)',
        Mode.arrowKey => 'Arrow key (Left/Right/Up/Down)',
        Mode.navKey => 'Nav key (Home/End/PageUp/PageDown/Insert)',
        Mode.functionKey => 'Function key (F1..F24)',
        Mode.tabKey => 'Tab',
        Mode.escapeKey => 'Escape',
        Mode.enterKey => 'Enter / Return',
        Mode.spaceKey => 'Space',
        Mode.backspaceKey => 'Backspace',
        Mode.syncKeyboardState =>
          'HardwareKeyboard.syncKeyboardState() invoked',
        Mode.clearStateCalled => 'HardwareKeyboard.clearState() invoked',
      };
}

class CoverageState extends ChangeNotifier {
  final Set<Mode> _observed = <Mode>{};

  Set<Mode> get observed => Set.unmodifiable(_observed);

  bool isObserved(Mode m) => _observed.contains(m);

  void mark(Mode m) {
    if (_observed.add(m)) {
      notifyListeners();
    }
  }

  void reset() {
    _observed.clear();
    notifyListeners();
  }

  Map<String, dynamic> toJson() => {
        'covered': _observed.map((m) => m.name).toList()..sort(),
        'missing': Mode.values
            .where((m) => !_observed.contains(m))
            .map((m) => m.name)
            .toList()
          ..sort(),
        'total': Mode.values.length,
        'coveredCount': _observed.length,
      };
}

// ---------------------------------------------------------------------------
// Home
// ---------------------------------------------------------------------------

class _Home extends StatefulWidget {
  const _Home();

  @override
  State<_Home> createState() => _HomeState();
}

class _HomeState extends State<_Home> {
  final CoverageState _coverage = CoverageState();
  final List<_KeyEventRecord> _eventLog = <_KeyEventRecord>[];
  static const int _maxLogEntries = 48;

  // Snapshot taken on the most recent dispatch. We capture into local fields
  // so the UI can render exactly what HardwareKeyboard reported at event time
  // without re-querying it during build.
  Set<PhysicalKeyboardKey> _physicalPressed = <PhysicalKeyboardKey>{};
  Set<LogicalKeyboardKey> _logicalPressed = <LogicalKeyboardKey>{};
  Set<KeyboardLockMode> _lockModes = <KeyboardLockMode>{};
  bool _shift = false;
  bool _ctrl = false;
  bool _alt = false;
  bool _meta = false;

  @override
  void initState() {
    super.initState();
    HardwareKeyboard.instance.addHandler(_handleKeyEvent);
    _refreshSnapshot();
  }

  @override
  void dispose() {
    HardwareKeyboard.instance.removeHandler(_handleKeyEvent);
    super.dispose();
  }

  // ---------- HardwareKeyboard handler ----------

  bool _handleKeyEvent(KeyEvent event) {
    final hk = HardwareKeyboard.instance;

    // Coverage: event type
    if (event is KeyDownEvent) {
      _coverage.mark(Mode.keyDown);
    } else if (event is KeyUpEvent) {
      _coverage.mark(Mode.keyUp);
    } else if (event is KeyRepeatEvent) {
      _coverage.mark(Mode.keyRepeat);
    }

    // Coverage: synthesized flag
    if (event.synthesized) {
      _coverage.mark(Mode.synthesizedEvent);
    } else {
      _coverage.mark(Mode.realEvent);
    }

    // Coverage: modifier convenience getters
    if (hk.isShiftPressed) _coverage.mark(Mode.modShift);
    if (hk.isControlPressed) _coverage.mark(Mode.modCtrl);
    if (hk.isAltPressed) _coverage.mark(Mode.modAlt);
    if (hk.isMetaPressed) _coverage.mark(Mode.modMeta);

    // Coverage: lock modes
    if (hk.lockModesEnabled.contains(KeyboardLockMode.capsLock)) {
      _coverage.mark(Mode.lockCaps);
    }
    if (hk.lockModesEnabled.contains(KeyboardLockMode.numLock)) {
      _coverage.mark(Mode.lockNum);
    }
    if (hk.lockModesEnabled.contains(KeyboardLockMode.scrollLock)) {
      _coverage.mark(Mode.lockScroll);
    }

    // Coverage: pressed-set tracking
    if (hk.physicalKeysPressed.isNotEmpty) {
      _coverage.mark(Mode.physicalKeyTracked);
    }
    if (hk.logicalKeysPressed.isNotEmpty) {
      _coverage.mark(Mode.logicalKeyTracked);
    }
    if (hk.physicalKeysPressed.length >= 2) {
      _coverage.mark(Mode.multiKeyChord);
    }

    // Coverage: character delivery
    final ch = event.character;
    if (ch != null && ch.isNotEmpty) {
      _coverage.mark(Mode.characterReceived);
      for (final r in ch.runes) {
        if (r > 0x7F) {
          _coverage.mark(Mode.nonAsciiCharacter);
          break;
        }
      }
    }

    // Coverage: logical key categories. Apply only on down/repeat so that a
    // single press doesn't double-mark via the up event.
    if (event is! KeyUpEvent) {
      _markByLogicalKey(event.logicalKey);
    }
    if (_isModifierLogicalKey(event.logicalKey)) {
      _coverage.mark(Mode.modifierKeyEvent);
    }

    // Update visible state and event log.
    setState(() {
      _eventLog.insert(0, _KeyEventRecord(event: event, snapshotAt: hk));
      if (_eventLog.length > _maxLogEntries) {
        _eventLog.removeLast();
      }
      _refreshSnapshot();
    });

    // We are an observer; never claim to have handled the event. Returning
    // false also keeps the test honest about *not* swallowing input.
    return false;
  }

  void _refreshSnapshot() {
    final hk = HardwareKeyboard.instance;
    _physicalPressed = Set<PhysicalKeyboardKey>.from(hk.physicalKeysPressed);
    _logicalPressed = Set<LogicalKeyboardKey>.from(hk.logicalKeysPressed);
    _lockModes = Set<KeyboardLockMode>.from(hk.lockModesEnabled);
    _shift = hk.isShiftPressed;
    _ctrl = hk.isControlPressed;
    _alt = hk.isAltPressed;
    _meta = hk.isMetaPressed;
  }

  void _markByLogicalKey(LogicalKeyboardKey k) {
    if (_isAsciiLetter(k)) {
      _coverage.mark(Mode.printableLetter);
    } else if (_isDigit(k)) {
      _coverage.mark(Mode.printableDigit);
    } else if (k == LogicalKeyboardKey.arrowLeft ||
        k == LogicalKeyboardKey.arrowRight ||
        k == LogicalKeyboardKey.arrowUp ||
        k == LogicalKeyboardKey.arrowDown) {
      _coverage.mark(Mode.arrowKey);
    } else if (k == LogicalKeyboardKey.home ||
        k == LogicalKeyboardKey.end ||
        k == LogicalKeyboardKey.pageUp ||
        k == LogicalKeyboardKey.pageDown ||
        k == LogicalKeyboardKey.insert) {
      _coverage.mark(Mode.navKey);
    } else if (_isFunctionKey(k)) {
      _coverage.mark(Mode.functionKey);
    } else if (k == LogicalKeyboardKey.tab) {
      _coverage.mark(Mode.tabKey);
    } else if (k == LogicalKeyboardKey.escape) {
      _coverage.mark(Mode.escapeKey);
    } else if (k == LogicalKeyboardKey.enter ||
        k == LogicalKeyboardKey.numpadEnter) {
      _coverage.mark(Mode.enterKey);
    } else if (k == LogicalKeyboardKey.space) {
      _coverage.mark(Mode.spaceKey);
    } else if (k == LogicalKeyboardKey.backspace) {
      _coverage.mark(Mode.backspaceKey);
    }
  }

  static bool _isAsciiLetter(LogicalKeyboardKey k) {
    final id = k.keyId;
    return (id >= 0x61 && id <= 0x7a) || (id >= 0x41 && id <= 0x5a);
  }

  static bool _isDigit(LogicalKeyboardKey k) {
    final id = k.keyId;
    return id >= 0x30 && id <= 0x39;
  }

  static bool _isFunctionKey(LogicalKeyboardKey k) {
    final id = k.keyId;
    return id >= LogicalKeyboardKey.f1.keyId &&
        id <= LogicalKeyboardKey.f24.keyId;
  }

  static bool _isModifierLogicalKey(LogicalKeyboardKey k) {
    return k == LogicalKeyboardKey.shift ||
        k == LogicalKeyboardKey.shiftLeft ||
        k == LogicalKeyboardKey.shiftRight ||
        k == LogicalKeyboardKey.control ||
        k == LogicalKeyboardKey.controlLeft ||
        k == LogicalKeyboardKey.controlRight ||
        k == LogicalKeyboardKey.alt ||
        k == LogicalKeyboardKey.altLeft ||
        k == LogicalKeyboardKey.altRight ||
        k == LogicalKeyboardKey.meta ||
        k == LogicalKeyboardKey.metaLeft ||
        k == LogicalKeyboardKey.metaRight ||
        k == LogicalKeyboardKey.capsLock ||
        k == LogicalKeyboardKey.numLock ||
        k == LogicalKeyboardKey.scrollLock;
  }

  // ---------- Programmatic API exercises ----------

  Future<void> _doSync() async {
    await HardwareKeyboard.instance.syncKeyboardState();
    _coverage.mark(Mode.syncKeyboardState);
    setState(() {
      _refreshSnapshot();
    });
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text('syncKeyboardState() completed')),
    );
  }

  void _doClearState() {
    HardwareKeyboard.instance.clearState();
    _coverage.mark(Mode.clearStateCalled);
    setState(() {
      _refreshSnapshot();
    });
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text('clearState() invoked — pressed sets reset')),
    );
  }

  void _printSelfCheck() {
    final summary = _coverage.toJson();
    // ignore: avoid_print
    print('HARDWARE_KEYBOARD_TEST_SUMMARY ${jsonEncode(summary)}');
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(
        content: Text(
          'Covered ${summary['coveredCount']}/${summary['total']} modes — '
          'see stdout for JSON',
        ),
      ),
    );
  }

  // ---------- UI ----------

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('ivi-homescreen HardwareKeyboard test'),
        actions: [
          IconButton(
            tooltip: 'syncKeyboardState()',
            icon: const Icon(Icons.sync),
            onPressed: _doSync,
          ),
          IconButton(
            tooltip: 'clearState()',
            icon: const Icon(Icons.layers_clear),
            onPressed: _doClearState,
          ),
          IconButton(
            tooltip: 'Reset coverage',
            icon: const Icon(Icons.refresh),
            onPressed: () => _coverage.reset(),
          ),
          IconButton(
            tooltip: 'Print self-check JSON to stdout',
            icon: const Icon(Icons.fact_check_outlined),
            onPressed: _printSelfCheck,
          ),
        ],
      ),
      // A focusable surface so the embedder routes key events to the engine.
      // No Shortcuts/Actions wired — the Focus is purely a target for input;
      // every observation flows through HardwareKeyboard.instance.
      body: Focus(
        autofocus: true,
        // Always swallow at the Focus level — but we still get the events on
        // HardwareKeyboard, which fires its handlers before any focus routing.
        // Returning ignored keeps Flutter from looking for a Shortcut binding.
        onKeyEvent: (_, __) => KeyEventResult.ignored,
        child: Padding(
          padding: const EdgeInsets.all(12.0),
          child: Row(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Expanded(
                flex: 3,
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Expanded(
                      flex: 3,
                      child: _LiveLogPanel(events: _eventLog),
                    ),
                    const SizedBox(height: 12),
                    Expanded(
                      flex: 2,
                      child: _StatePanel(
                        physical: _physicalPressed,
                        logical: _logicalPressed,
                        locks: _lockModes,
                        shift: _shift,
                        ctrl: _ctrl,
                        alt: _alt,
                        meta: _meta,
                      ),
                    ),
                  ],
                ),
              ),
              const SizedBox(width: 12),
              Expanded(
                flex: 2,
                child: _ChecklistPanel(coverage: _coverage),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

// ---------------------------------------------------------------------------
// Live event log panel
// ---------------------------------------------------------------------------

@immutable
class _KeyEventRecord {
  _KeyEventRecord({required this.event, required HardwareKeyboard snapshotAt})
      : timestamp = DateTime.now(),
        modString = _renderMods(snapshotAt);

  final KeyEvent event;
  final DateTime timestamp;
  final String modString;

  String get typeLabel => switch (event) {
        KeyDownEvent _ => 'DOWN',
        KeyUpEvent _ => 'UP  ',
        KeyRepeatEvent _ => 'RPT ',
        _ => '?   ',
      };

  static String _renderMods(HardwareKeyboard hk) {
    final parts = <String>[
      if (hk.isShiftPressed) 'Shift',
      if (hk.isControlPressed) 'Ctrl',
      if (hk.isAltPressed) 'Alt',
      if (hk.isMetaPressed) 'Meta',
      if (hk.lockModesEnabled.contains(KeyboardLockMode.capsLock)) 'Caps',
      if (hk.lockModesEnabled.contains(KeyboardLockMode.numLock)) 'Num',
      if (hk.lockModesEnabled.contains(KeyboardLockMode.scrollLock)) 'Scroll',
    ];
    return parts.isEmpty ? '—' : parts.join('+');
  }
}

class _LiveLogPanel extends StatelessWidget {
  const _LiveLogPanel({required this.events});

  final List<_KeyEventRecord> events;

  @override
  Widget build(BuildContext context) {
    return _Card(
      title: 'HardwareKeyboard.addHandler stream (most recent first)',
      child: events.isEmpty
          ? const Center(
              child: Text(
                'Press any key…',
                style: TextStyle(color: Colors.white54),
              ),
            )
          : ListView.builder(
              itemCount: events.length,
              itemBuilder: (context, i) {
                final r = events[i];
                final ev = r.event;
                final char = (ev.character?.isNotEmpty ?? false)
                    ? _escapeChar(ev.character!)
                    : '—';
                final synth = ev.synthesized ? 'synth' : 'real ';
                return Padding(
                  padding: const EdgeInsets.symmetric(vertical: 2),
                  child: DefaultTextStyle.merge(
                    style: const TextStyle(
                      fontFamily: 'monospace',
                      fontSize: 12,
                    ),
                    child: Text(
                      '${r.typeLabel} $synth '
                      'phys=0x${ev.physicalKey.usbHidUsage.toRadixString(16).padLeft(8, '0')} '
                      'log=0x${ev.logicalKey.keyId.toRadixString(16).padLeft(8, '0')} '
                      'char=$char '
                      't=${ev.timeStamp.inMilliseconds}ms '
                      'dev=${ev.deviceType.name} '
                      'mods=${r.modString}',
                    ),
                  ),
                );
              },
            ),
    );
  }

  static String _escapeChar(String c) {
    if (c.length == 1) {
      final r = c.codeUnitAt(0);
      if (r < 0x20 || r == 0x7f) {
        return 'U+${r.toRadixString(16).padLeft(4, '0').toUpperCase()}';
      }
    }
    return c;
  }
}

// ---------------------------------------------------------------------------
// Live HardwareKeyboard state panel
// ---------------------------------------------------------------------------

class _StatePanel extends StatelessWidget {
  const _StatePanel({
    required this.physical,
    required this.logical,
    required this.locks,
    required this.shift,
    required this.ctrl,
    required this.alt,
    required this.meta,
  });

  final Set<PhysicalKeyboardKey> physical;
  final Set<LogicalKeyboardKey> logical;
  final Set<KeyboardLockMode> locks;
  final bool shift;
  final bool ctrl;
  final bool alt;
  final bool meta;

  @override
  Widget build(BuildContext context) {
    final physList = physical.map((k) => k.debugName ?? '0x${k.usbHidUsage.toRadixString(16)}').toList()..sort();
    final logList = logical.map((k) => k.debugName ?? '0x${k.keyId.toRadixString(16)}').toList()..sort();
    final lockList = locks.map((m) => m.name).toList()..sort();

    return _Card(
      title: 'HardwareKeyboard live state',
      child: SingleChildScrollView(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            _ModRow(label: 'isShiftPressed', value: shift),
            _ModRow(label: 'isControlPressed', value: ctrl),
            _ModRow(label: 'isAltPressed', value: alt),
            _ModRow(label: 'isMetaPressed', value: meta),
            const SizedBox(height: 8),
            _SetRow(label: 'lockModesEnabled (${lockList.length})', items: lockList),
            const SizedBox(height: 8),
            _SetRow(label: 'physicalKeysPressed (${physList.length})', items: physList),
            const SizedBox(height: 8),
            _SetRow(label: 'logicalKeysPressed (${logList.length})', items: logList),
          ],
        ),
      ),
    );
  }
}

class _ModRow extends StatelessWidget {
  const _ModRow({required this.label, required this.value});

  final String label;
  final bool value;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 2),
      child: Row(
        children: [
          Icon(
            value ? Icons.check_circle : Icons.radio_button_unchecked,
            size: 16,
            color: value ? Colors.tealAccent : Colors.white38,
          ),
          const SizedBox(width: 8),
          Text(
            label,
            style: const TextStyle(
              fontFamily: 'monospace',
              fontSize: 12,
            ),
          ),
        ],
      ),
    );
  }
}

class _SetRow extends StatelessWidget {
  const _SetRow({required this.label, required this.items});

  final String label;
  final List<String> items;

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(
          label,
          style: const TextStyle(
            fontFamily: 'monospace',
            fontSize: 12,
            fontWeight: FontWeight.w600,
          ),
        ),
        const SizedBox(height: 2),
        Text(
          items.isEmpty ? '  (empty)' : '  ${items.join(', ')}',
          style: const TextStyle(
            fontFamily: 'monospace',
            fontSize: 11,
            color: Colors.white70,
          ),
        ),
      ],
    );
  }
}

// ---------------------------------------------------------------------------
// Coverage checklist panel
// ---------------------------------------------------------------------------

class _ChecklistPanel extends StatelessWidget {
  const _ChecklistPanel({required this.coverage});

  final CoverageState coverage;

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: coverage,
      builder: (context, _) {
        final covered = coverage.observed.length;
        final total = Mode.values.length;
        return _Card(
          title: 'Coverage — $covered / $total',
          child: ListView(
            children: [
              for (final mode in Mode.values)
                _ChecklistRow(
                  label: mode.label,
                  observed: coverage.isObserved(mode),
                ),
            ],
          ),
        );
      },
    );
  }
}

class _ChecklistRow extends StatelessWidget {
  const _ChecklistRow({required this.label, required this.observed});

  final String label;
  final bool observed;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: Row(
        children: [
          Icon(
            observed ? Icons.check_circle : Icons.radio_button_unchecked,
            size: 18,
            color: observed ? Colors.tealAccent : Colors.white38,
          ),
          const SizedBox(width: 10),
          Expanded(
            child: Text(
              label,
              style: TextStyle(
                color: observed ? Colors.white : Colors.white70,
                fontWeight: observed ? FontWeight.w600 : FontWeight.normal,
              ),
            ),
          ),
        ],
      ),
    );
  }
}

// ---------------------------------------------------------------------------
// Common card chrome
// ---------------------------------------------------------------------------

class _Card extends StatelessWidget {
  const _Card({required this.title, required this.child});

  final String title;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        color: Colors.white.withValues(alpha: 0.04),
        border: Border.all(color: Colors.white12),
        borderRadius: BorderRadius.circular(8),
      ),
      child: Padding(
        padding: const EdgeInsets.all(10),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text(
              title,
              style: const TextStyle(
                fontWeight: FontWeight.w600,
                fontSize: 13,
                letterSpacing: 0.3,
              ),
            ),
            const Divider(height: 12),
            Expanded(child: child),
          ],
        ),
      ),
    );
  }
}