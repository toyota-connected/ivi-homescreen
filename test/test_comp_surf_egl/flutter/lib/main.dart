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

import 'dart:ffi';

import 'package:flutter/material.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter/services.dart';
import 'dart:math' as math;

const MethodChannel channel = MethodChannel('comp_surf');

void main() {
  runApp(const MyApp());
}

Widget text(String text) {
  return Stack(
    children: <Widget>[
      Text(
        text,
        style: TextStyle(
          foreground: Paint()
            ..style = PaintingStyle.stroke
            ..strokeWidth = 3
            ..color = const Color.fromARGB(255, 0, 0, 0),
        ),
      ),
      Text(
        text,
        style: const TextStyle(
          color: Color.fromARGB(255, 255, 255, 255),
        ),
      ),
    ],
  );
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
    return WidgetsApp(
      color: const Color(0x00000000),
      debugShowCheckedModeBanner: false,
      builder: (context, _) => Container(
        decoration: BoxDecoration(
            color: const Color.fromARGB(255, 129, 129, 129),
            borderRadius: BorderRadius.circular(5)),
        child: Column(
          children: [
            Expanded(
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  Expanded(
                      child: SurfaceInstance(
                          name: 'Surface(0)',
                          view: 0,
                          type: 'egl',
                          z_order: 'above',
                          sync: 'sync',
                          x: 10,
                          y: 10,
                          width: 800,
                          height: 600,
                          module: 'libcomp_surf_egl.so',
                          asset_path: '/usr/local/share/comp_surf_egl/assets',
                          cache_folder: '.config/comp_surf_egl/instance0',
                          misc_folder: '.config/comp_surf_egl/common'
                      ),
                  ),
                ],
              ),
            ),
            Expanded(
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  Expanded(
                      child: SurfaceInstance(
                          name: 'Surface(1)',
                          view: 0,
                          type: 'vulkan',
                          z_order: 'above',
                          sync: 'sync',
                          x: 820,
                          y: 10,
                          width: 800,
                          height: 600,
                          module: 'libcomp_surf_vulkan.so',
                          asset_path: '/usr/local/share/comp_surf_vulkan/assets',
                          cache_folder: '.config/comp_surf_vulkan/instance0',
                          misc_folder: '.config/comp_surf_vulkan/common'
                      ),
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class SurfaceInstance extends StatefulWidget {
  SurfaceInstance(
      {super.key,
      required this.name,
      required this.view,
      required this.type,
      required this.z_order,
      required this.sync,
      required this.x,
      required this.y,
      required this.width,
      required this.height,
      required this.module,
      required this.asset_path,
      required this.cache_folder,
      required this.misc_folder,
      });

  final String name;
  final int view;
  final String type;
  final String z_order;
  final String sync;
  final int x, y;
  final int width, height;
  final String module;
  final String asset_path;
  final String cache_folder;
  final String misc_folder;

  @override
  State<SurfaceInstance> createState() => _SurfaceInstanceState();
}

class _SurfaceInstanceState extends State<SurfaceInstance> {
  int? result;
  int? surfaceCtx;
  int? surfaceIdx;
  Map<dynamic, dynamic>? response;

  bool get created => surfaceCtx != null;

  @override
  Widget build(BuildContext context) {
    return Container(
      margin: const EdgeInsets.all(5),
      child: LayoutBuilder(
        builder: (context, constraints) {
          return Stack(
            children: [
              Align(
                alignment: Alignment.centerRight,
                child: GestureDetector(
                  behavior: HitTestBehavior.opaque,
                  onTap: () {
                    if (!created) {
                      final req = {
                        'view': widget.view,
                        'type': widget.type,
                        'z_order': widget.z_order,
                        'sync': widget.sync,
                        'x': widget.x,
                        'y': widget.y,
                        'width': widget.width,
                        'height': widget.height,
                        'module': widget.module,
                        'assets_path': widget.asset_path,
                        'cache_folder': widget.cache_folder,
                        'misc_folder': widget.misc_folder,
                      };
                      print('Create request: $req');
                      channel.invokeMethod('create', req).then((resp) {
                        print('Create response: $resp');
                        setState(() {
                          result = resp?['result'] as int?;
                          surfaceCtx = resp?['context'] as int?;
                          surfaceIdx = resp?['index'] as int?;
                          response = resp;
                        });
                      }).catchError((Object e) {
                        print('Create error: $e');
                      });
                    } else {
                      final req = {
                        'view': widget.view,
                        'index': surfaceIdx,
                      };
                      print('Dispose request: $req');
                      channel.invokeMethod('dispose', req).then((resp) {
                        print('Dispose response: $resp');
                        setState(() {
                          surfaceCtx = surfaceIdx = response = result = null;
                        });
                      });
                    }
                  },
                  child: Container(
                    decoration: BoxDecoration(
                        color: created
                            ? const Color.fromARGB(255, 175, 102, 43)
                            : const Color.fromARGB(255, 43, 175, 43),
                        borderRadius:
                            const BorderRadius.all(Radius.circular(16)),
                        border: Border.all(
                            width: 3,
                            color: const Color.fromARGB(255, 0, 0, 0))),
                    width: 150,
                    height: 75,
                    child: Center(
                        child: text(created
                            ? 'Destroy ${widget.name}'
                            : 'Create ${widget.name}')),
                  ),
                ),
              ),
            ],
          );
        },
      ),
    );
  }
}
