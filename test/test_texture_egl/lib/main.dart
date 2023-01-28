import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'dart:math' as math;

const testTextureID = 5150;
const MethodChannel channel = MethodChannel('opengl_texture');

void main() {
  runApp(const MyApp());
}

class MyApp extends StatefulWidget {
  const MyApp({Key? key}) : super(key: key);

  @override
  State<MyApp> createState() => _MyAppState();
}

class _MyAppState extends State<MyApp> {
  int? textureId;
  bool createCalled = false;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      title: 'Texture Test',
      home: LayoutBuilder(
        builder: (context, constraints) {
          if (!createCalled) {
            createCalled = true;
            channel.invokeMethod<Map<dynamic, dynamic>>(
              'create',
              {
                'textureId': testTextureID,
                'width': constraints.maxWidth,
                'height': constraints.maxHeight,
              },
            ).then((response) {
              setState(() => textureId = response!['textureId'] as int);
            });
          }

          if (textureId == null) {
            return const ColoredBox(color: Colors.red);
          } else {
            return Transform(
              alignment: Alignment.center,
              transform: Matrix4.rotationX(math.pi),
              child: Texture(textureId: textureId!),
            );
          }
        },
      ),
    );
  }
}
