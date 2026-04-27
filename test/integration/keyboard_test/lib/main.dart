// Integration test app for the ivi-homescreen keyboard pipeline.
//
// Three panels exercise every keyboard mode the homescreen supports:
//
//   1. Live key-event log
//      Hooks HardwareKeyboard (the embedder-API / FlutterEngineSendKeyEvent
//      path) and shows raw KeyDownEvent / KeyUpEvent / KeyRepeatEvent with
//      physicalKey, logicalKey, character, and the active modifier set.
//
//   2. Text input fields
//      A single-line and a multi-line TextField drive the legacy
//      flutter/textinput channel — text insertion, Backspace/Delete, cursor
//      navigation including Up/Down across lines, Enter (action vs newline),
//      and Shift-extended selection.
//
//   3. Coverage checklist
//      Every mode the test is meant to exercise. Items flip green the first
//      time they're observed, so a tester (or automation) can see at a
//      glance what has been covered. Tap "Reset" to clear; tap "Self-check"
//      to dump a JSON summary suitable for CI assertions.

import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

void main() {
  FlutterError.onError = (FlutterErrorDetails details) {
    FlutterError.dumpErrorToConsole(details);
    if (details.stack != null) {
      debugPrint('STACK:\n${details.stack}');
    }
  };
  runApp(const KeyboardTestApp());
}

class KeyboardTestApp extends StatelessWidget {
  const KeyboardTestApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'ivi-homescreen keyboard test',
      theme: ThemeData(
        colorSchemeSeed: Colors.indigo,
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

/// Every mode the integration test cares about. Names are short so they
/// render in the checklist; the longer description goes in [Mode.label].
enum Mode {
  keyDown,
  keyUp,
  keyRepeat,
  modShift,
  modCtrl,
  modAlt,
  modCapsLock,
  modNumLock,
  modLogo,
  letterTyped,
  nonAsciiTyped,
  backspace,
  deleteKey,
  arrowLeftRight,
  arrowUpDownAcrossLines,
  homeEnd,
  pageUpDown,
  functionKey,
  tabKey,
  escapeKey,
  enterAction,
  multilineNewline,
  shiftSelection,
  ctrlShortcut,
}

extension on Mode {
  String get label => switch (this) {
        Mode.keyDown => 'Key down event',
        Mode.keyUp => 'Key up event',
        Mode.keyRepeat => 'Key repeat event',
        Mode.modShift => 'Shift modifier seen',
        Mode.modCtrl => 'Ctrl modifier seen',
        Mode.modAlt => 'Alt modifier seen',
        Mode.modCapsLock => 'Caps Lock modifier seen',
        Mode.modNumLock => 'Num Lock modifier seen',
        Mode.modLogo => 'Logo / Meta modifier seen',
        Mode.letterTyped => 'Printable ASCII letter typed',
        Mode.nonAsciiTyped => 'Non-ASCII character typed',
        Mode.backspace => 'Backspace deleted a character',
        Mode.deleteKey => 'Delete removed a character',
        Mode.arrowLeftRight => 'Left/Right arrow moved cursor',
        Mode.arrowUpDownAcrossLines =>
          'Up/Down arrow moved cursor across lines',
        Mode.homeEnd => 'Home/End moved cursor',
        Mode.pageUpDown => 'Page Up / Page Down received',
        Mode.functionKey => 'Function key (F1..F24) received',
        Mode.tabKey => 'Tab key received',
        Mode.escapeKey => 'Escape key received',
        Mode.enterAction => 'Enter triggered TextInputAction',
        Mode.multilineNewline => 'Enter inserted newline (multiline)',
        Mode.shiftSelection => 'Shift+Arrow extended selection',
        Mode.ctrlShortcut => 'Ctrl+letter shortcut intercepted',
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

  /// JSON summary suitable for CI assertion or logging.
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
  static const int _maxLogEntries = 32;

  final TextEditingController _singleLineController =
      TextEditingController(text: 'edit me');
  final TextEditingController _multiLineController = TextEditingController(
    text: 'line one\nsecond line\nthird line with é and ö\nfinal line',
  );
  String _previousSingle = 'edit me';
  String _previousMulti = '';
  TextSelection _previousMultiSelection = const TextSelection.collapsed(
    offset: 0,
  );
  int _previousMultiLineCount = 0;

  @override
  void initState() {
    super.initState();
    HardwareKeyboard.instance.addHandler(_handleHardwareKey);
    _previousMulti = _multiLineController.text;
    _previousMultiSelection = _multiLineController.selection;
    _previousMultiLineCount = '\n'.allMatches(_previousMulti).length + 1;
    _singleLineController.addListener(_observeSingleLine);
    _multiLineController.addListener(_observeMultiLine);
  }

  @override
  void dispose() {
    HardwareKeyboard.instance.removeHandler(_handleHardwareKey);
    _singleLineController.dispose();
    _multiLineController.dispose();
    super.dispose();
  }

  // ---------- Hardware key event observation ----------

  bool _handleHardwareKey(KeyEvent event) {
    setState(() {
      _eventLog.insert(0, _KeyEventRecord(event: event));
      if (_eventLog.length > _maxLogEntries) {
        _eventLog.removeLast();
      }
    });

    if (event is KeyDownEvent) {
      _coverage.mark(Mode.keyDown);
    } else if (event is KeyUpEvent) {
      _coverage.mark(Mode.keyUp);
    } else if (event is KeyRepeatEvent) {
      _coverage.mark(Mode.keyRepeat);
    }

    final mods = HardwareKeyboard.instance;
    if (mods.isShiftPressed) _coverage.mark(Mode.modShift);
    if (mods.isControlPressed) _coverage.mark(Mode.modCtrl);
    if (mods.isAltPressed) _coverage.mark(Mode.modAlt);
    if (mods.lockModesEnabled.contains(KeyboardLockMode.capsLock)) {
      _coverage.mark(Mode.modCapsLock);
    }
    if (mods.lockModesEnabled.contains(KeyboardLockMode.numLock)) {
      _coverage.mark(Mode.modNumLock);
    }
    if (mods.isMetaPressed) _coverage.mark(Mode.modLogo);

    final logical = event.logicalKey;
    if (event is! KeyUpEvent) {
      if (logical == LogicalKeyboardKey.backspace) {
        _coverage.mark(Mode.backspace);
      } else if (logical == LogicalKeyboardKey.delete) {
        _coverage.mark(Mode.deleteKey);
      } else if (logical == LogicalKeyboardKey.arrowLeft ||
          logical == LogicalKeyboardKey.arrowRight) {
        _coverage.mark(Mode.arrowLeftRight);
        if (mods.isShiftPressed) _coverage.mark(Mode.shiftSelection);
      } else if (logical == LogicalKeyboardKey.home ||
          logical == LogicalKeyboardKey.end) {
        _coverage.mark(Mode.homeEnd);
      } else if (logical == LogicalKeyboardKey.pageUp ||
          logical == LogicalKeyboardKey.pageDown) {
        _coverage.mark(Mode.pageUpDown);
      } else if (logical == LogicalKeyboardKey.tab) {
        _coverage.mark(Mode.tabKey);
      } else if (logical == LogicalKeyboardKey.escape) {
        _coverage.mark(Mode.escapeKey);
      } else if (_isFunctionKey(logical)) {
        _coverage.mark(Mode.functionKey);
      } else if (mods.isControlPressed && _isAsciiLetter(logical)) {
        _coverage.mark(Mode.ctrlShortcut);
      }
    }

    // Don't claim handled — let the event continue to TextField focus.
    return false;
  }

  static bool _isFunctionKey(LogicalKeyboardKey k) {
    final id = k.keyId;
    return id >= LogicalKeyboardKey.f1.keyId &&
        id <= LogicalKeyboardKey.f24.keyId;
  }

  static bool _isAsciiLetter(LogicalKeyboardKey k) {
    final id = k.keyId;
    return (id >= 0x61 && id <= 0x7a) || (id >= 0x41 && id <= 0x5a);
  }

  // ---------- Text input observation ----------

  void _observeSingleLine() {
    final text = _singleLineController.text;
    if (text != _previousSingle) {
      if (text.length > _previousSingle.length) {
        final added = text.substring(_previousSingle.length);
        for (final r in added.runes) {
          if (r >= 0x20 && r != 0x7f) {
            if (r > 0x7f) {
              _coverage.mark(Mode.nonAsciiTyped);
            } else if (_isAsciiLetterCodepoint(r)) {
              _coverage.mark(Mode.letterTyped);
            }
          }
        }
      }
      _previousSingle = text;
    }
  }

  void _observeMultiLine() {
    final text = _multiLineController.text;
    final selection = _multiLineController.selection;

    if (text != _previousMulti) {
      final newLines = '\n'.allMatches(text).length + 1;
      if (newLines > _previousMultiLineCount) {
        _coverage.mark(Mode.multilineNewline);
      }
      _previousMultiLineCount = newLines;
      _previousMulti = text;
    }

    // Detect Up/Down by line change without text change.
    if (selection.isCollapsed && _previousMultiSelection.isCollapsed) {
      final prevLine = _lineOfOffset(_previousMulti, _previousMultiSelection.baseOffset);
      final newLine = _lineOfOffset(text, selection.baseOffset);
      if (prevLine != newLine && text == _previousMulti) {
        _coverage.mark(Mode.arrowUpDownAcrossLines);
      }
    }
    _previousMultiSelection = selection;
  }

  static int _lineOfOffset(String text, int offset) {
    if (offset < 0) return 0;
    final clamped = offset > text.length ? text.length : offset;
    return '\n'.allMatches(text.substring(0, clamped)).length;
  }

  static bool _isAsciiLetterCodepoint(int r) =>
      (r >= 0x41 && r <= 0x5a) || (r >= 0x61 && r <= 0x7a);

  void _onSingleLineSubmitted(String _) {
    _coverage.mark(Mode.enterAction);
  }

  void _printSelfCheck() {
    final summary = _coverage.toJson();
    // ignore: avoid_print
    print('KEYBOARD_TEST_SUMMARY ${jsonEncode(summary)}');
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
        title: const Text('ivi-homescreen keyboard test'),
        actions: [
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
      body: Padding(
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
                    flex: 2,
                    child: _LiveLogPanel(events: _eventLog),
                  ),
                  const SizedBox(height: 12),
                  Expanded(
                    flex: 1,
                    child: _TextInputPanel(
                      singleLineController: _singleLineController,
                      multiLineController: _multiLineController,
                      onSingleLineSubmitted: _onSingleLineSubmitted,
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
    );
  }
}

// ---------------------------------------------------------------------------
// Live event log panel
// ---------------------------------------------------------------------------

@immutable
class _KeyEventRecord {
  _KeyEventRecord({required this.event}) : timestamp = DateTime.now();

  final KeyEvent event;
  final DateTime timestamp;

  String get typeLabel => switch (event) {
        KeyDownEvent _ => 'DOWN',
        KeyUpEvent _ => 'UP  ',
        KeyRepeatEvent _ => 'RPT ',
        _ => '?   ',
      };

  String get modifierString {
    final mods = HardwareKeyboard.instance;
    final parts = <String>[
      if (mods.isShiftPressed) 'Shift',
      if (mods.isControlPressed) 'Ctrl',
      if (mods.isAltPressed) 'Alt',
      if (mods.isMetaPressed) 'Logo',
      if (mods.lockModesEnabled.contains(KeyboardLockMode.capsLock)) 'Caps',
      if (mods.lockModesEnabled.contains(KeyboardLockMode.numLock)) 'Num',
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
      title: 'HardwareKeyboard events (most recent first)',
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
                    ? ev.character
                    : '—';
                return Padding(
                  padding: const EdgeInsets.symmetric(vertical: 2),
                  child: DefaultTextStyle.merge(
                    style: const TextStyle(
                      fontFamily: 'monospace',
                      fontSize: 12,
                    ),
                    child: Text(
                      '${r.typeLabel}  '
                      'phys=0x${ev.physicalKey.usbHidUsage.toRadixString(16).padLeft(8, '0')}  '
                      'log=0x${ev.logicalKey.keyId.toRadixString(16).padLeft(8, '0')}  '
                      'char=$char  '
                      'mods=${r.modifierString}',
                    ),
                  ),
                );
              },
            ),
    );
  }
}

// ---------------------------------------------------------------------------
// Text input panel
// ---------------------------------------------------------------------------

class _TextInputPanel extends StatelessWidget {
  const _TextInputPanel({
    required this.singleLineController,
    required this.multiLineController,
    required this.onSingleLineSubmitted,
  });

  final TextEditingController singleLineController;
  final TextEditingController multiLineController;
  final ValueChanged<String> onSingleLineSubmitted;

  @override
  Widget build(BuildContext context) {
    return _Card(
      title: 'TextInputPlugin — type, navigate, edit',
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          TextField(
            controller: singleLineController,
            decoration: const InputDecoration(
              labelText: 'Single line — Enter sends action',
              border: OutlineInputBorder(),
            ),
            textInputAction: TextInputAction.done,
            onSubmitted: onSingleLineSubmitted,
          ),
          const SizedBox(height: 12),
          Expanded(
            child: TextField(
              controller: multiLineController,
              decoration: const InputDecoration(
                labelText: 'Multi-line — Enter inserts newline; Up/Down navigate',
                border: OutlineInputBorder(),
                alignLabelWithHint: true,
              ),
              maxLines: null,
              expands: true,
              textAlignVertical: TextAlignVertical.top,
              keyboardType: TextInputType.multiline,
            ),
          ),
        ],
      ),
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
            color: observed ? Colors.greenAccent : Colors.white38,
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
        color: Colors.white.withOpacity(0.04),
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
