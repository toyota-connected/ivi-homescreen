import 'package:flutter/material.dart';

import 'mouse_test_frame.dart';
import 'text_input_panel.dart';

void main() {
  runApp(const MyApp());
}

class MyApp extends StatelessWidget {
  const MyApp({super.key});

  @override
  Widget build(BuildContext context) {
    return const MaterialApp(
      title: 'Gesture Playground',
      debugShowCheckedModeBanner: false,
      home: ColoredBox(
        color: Color(0xFF2B2B2B),
        child: SafeArea(
          child: Column(
            children: <Widget>[
              Expanded(child: MouseTestFrame()),
              TextInputPanel(),
            ],
          ),
        ),
      ),
    );
  }
}
