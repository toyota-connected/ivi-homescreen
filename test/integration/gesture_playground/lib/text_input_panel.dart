import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

/// Intent fired by the Ctrl+K shortcut to clear the text field.
class ClearTextIntent extends Intent {
  ///
  const ClearTextIntent();
}

/// A focusable text-input surface for exercising the embedder's text-input /
/// IME path (`TextInputConnection`, composing region, surrounding text) and a
/// `Shortcuts` / `Actions` / `Intents` binding (Ctrl+K clears the field).
///
/// The readout below the field shows the live length, selection, and composing
/// region so IME state is visible while typing.
class TextInputPanel extends StatefulWidget {
  ///
  const TextInputPanel({super.key});

  @override
  State<TextInputPanel> createState() => _TextInputPanelState();
}

class _TextInputPanelState extends State<TextInputPanel> {
  final TextEditingController _controller = TextEditingController();
  final FocusNode _focusNode = FocusNode();

  @override
  void initState() {
    super.initState();
    _controller.addListener(_onChanged);
  }

  @override
  void dispose() {
    _controller
      ..removeListener(_onChanged)
      ..dispose();
    _focusNode.dispose();
    super.dispose();
  }

  void _onChanged() => setState(() {});

  String get _status {
    final value = _controller.value;
    final composing = value.composing;
    final composingText = composing.isValid
        ? value.text.substring(composing.start, composing.end)
        : '';
    return 'len=${value.text.length}  '
        'sel=[${value.selection.start},${value.selection.end}]  '
        'composing=${composing.isValid ? '"$composingText"' : 'none'}';
  }

  @override
  Widget build(BuildContext context) {
    return Shortcuts(
      shortcuts: const <ShortcutActivator, Intent>{
        SingleActivator(LogicalKeyboardKey.keyK, control: true):
            ClearTextIntent(),
      },
      child: Actions(
        actions: <Type, Action<Intent>>{
          ClearTextIntent: CallbackAction<ClearTextIntent>(
            onInvoke: (intent) {
              _controller.clear();
              return null;
            },
          ),
        },
        child: Material(
          type: MaterialType.transparency,
          child: DecoratedBox(
            decoration: const BoxDecoration(color: Color(0xFF1E1E1E)),
            child: Padding(
              padding: const EdgeInsets.all(8),
              child: Column(
                mainAxisSize: MainAxisSize.min,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: <Widget>[
                  TextField(
                    controller: _controller,
                    focusNode: _focusNode,
                    style: const TextStyle(color: Color(0xFFFFFFFF)),
                    decoration: const InputDecoration(
                      isDense: true,
                      hintText: 'IME / text-input test — Ctrl+K clears',
                      hintStyle: TextStyle(color: Color(0x88FFFFFF)),
                    ),
                  ),
                  const SizedBox(height: 4),
                  Text(
                    _status,
                    style: const TextStyle(
                      color: Color(0xFFB0B0B0),
                      fontFamily: 'monospace',
                      fontSize: 12,
                    ),
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}
