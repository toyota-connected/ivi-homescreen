import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:gesture_playground/text_input_panel.dart';

void main() {
  testWidgets('typing updates the readout', (WidgetTester tester) async {
    await tester.pumpWidget(
      const MaterialApp(home: Scaffold(body: TextInputPanel())),
    );

    await tester.enterText(find.byType(TextField), 'hello');
    await tester.pump();

    expect(find.textContaining('len=5'), findsOneWidget);
  });

  testWidgets('Ctrl+K clears the field', (WidgetTester tester) async {
    await tester.pumpWidget(
      const MaterialApp(home: Scaffold(body: TextInputPanel())),
    );

    await tester.enterText(find.byType(TextField), 'hello');
    await tester.pump();
    expect(find.textContaining('len=5'), findsOneWidget);

    await tester.sendKeyDownEvent(LogicalKeyboardKey.controlLeft);
    await tester.sendKeyEvent(LogicalKeyboardKey.keyK);
    await tester.sendKeyUpEvent(LogicalKeyboardKey.controlLeft);
    await tester.pump();

    expect(find.textContaining('len=0'), findsOneWidget);
    final field = tester.widget<TextField>(find.byType(TextField));
    expect(field.controller!.text, isEmpty);
  });
}
