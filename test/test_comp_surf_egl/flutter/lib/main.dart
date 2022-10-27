/*
* Copyright 2022 Toyota Connected North America
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';


const MethodChannel channel = MethodChannel('comp_surf');

void main() {
  runApp(const MyApp());
}

class MyApp extends StatefulWidget {
  const MyApp({Key? key}) : super(key: key);

  @override
  State<MyApp> createState() => _MyAppState();
}

class _MyAppState extends State<MyApp> {
  int? surfaceContext;
  bool createCalled = false;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      title: 'Compositor Surface Test',
      home: LayoutBuilder(
        builder: (context, constraints) {
          if (!createCalled) {
            createCalled = true;
            channel.invokeMethod<Map<dynamic, dynamic>>(
              'create',
              {
                'view': 0,
                'type': 'egl',
                'z_order': 'above',
                'sync': 'sync',
                'x': 10,
                'y': 10,
                'width': 800,
                'height': 600,
                'module': 'libcomp_surf_egl.so',
                'assets_path': '/usr/local/shared/test_comp_surf_egl/assets',
              },
            ).then((response) {
              setState(() => surfaceContext = response!['context'] as int);
            });
          }

          if (surfaceContext == null) {
            return const ColoredBox(color: Colors.red);
          } else {
            return const ColoredBox(color: Colors.blue);
          }
        },
      ),
    );
  }
}
