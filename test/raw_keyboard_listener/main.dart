import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

void main() {
  runApp(const TextPad());
}

class TextPad extends StatelessWidget {
  const TextPad({Key? key}) : super(key: key);

  // This widget is the root of your application.
  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Text Pad',
      theme: ThemeData(
        primaryColor: Colors.white,
      ),
      home: const Notebooks(title: 'title'),
    );
  }
}

class Notebooks extends StatefulWidget {
  const Notebooks({Key? key, required this.title}) : super(key: key);

  final String title;

  @override
  State<Notebooks> createState() => _NotebooksState();
}

String handleKeyboard(RawKeyEvent event) {

  print(event);

  var keyLabel = 
    event.logicalKey.keyLabel.isNotEmpty ? 
      event.logicalKey.keyLabel : 
      event.physicalKey.debugName.toString();

  var retString = "unknown";
  if (event is RawKeyDownEvent) {
    if (event.repeat) {
        retString = "KeyRepeat_" + keyLabel;
    }
    else {
        retString = "KeyDown_" + keyLabel;
    }
  } else if (event is RawKeyUpEvent) {
    retString = "KeyUp_" + keyLabel;
  }

  return retString;
}

class _NotebooksState extends State<Notebooks> {
  String _current = '';
  var _text = '';

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Edit'),
      ),
      body: Container(
        padding: const EdgeInsets.all(16.0),
        child: Focus(
            autofocus: true,
            onKey: (FocusNode node, RawKeyEvent event) {
              var retString = handleKeyboard(event);
              if (retString == "") {
                return KeyEventResult.ignored;
              } else if (retString.contains(RegExp("Key.*_[A-Z]\$"))) {
                return KeyEventResult.ignored;
              } else if (retString.contains(RegExp("Key.*_(Backspace|Enter|Tab| |Delete|Shift.*|Control.*)"))) {
                return KeyEventResult.ignored;
              } else {
                setState(() {
                  _text += '[' + retString + ']';
                });
                return KeyEventResult.handled;
              }
            },
            child: Column(
              children: <Widget>[
                TextField(
                  controller: TextEditingController(text: _current),
                  maxLines: 20,
                  style: const TextStyle(color: Colors.black),
                  onChanged: (text) {
                    _current = text;
                  },
                ),
                Text(_text),
              ],
            )),
      ),
    );
  }
}
