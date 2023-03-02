import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

void main() {
  runApp(const MyApp());
}

class MyApp extends StatelessWidget {
  const MyApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Flutter Demo',
      theme: ThemeData(
        primarySwatch: Colors.blue,
      ),
      home: const MyHomePage(title: 'Flutter Demo Home Page'),
    );
  }
}

class MyHomePage extends StatefulWidget {
  const MyHomePage({super.key, required this.title});

  final String title;

  @override
  State<MyHomePage> createState() => _MyHomePageState();
}

class _MyHomePageState extends State<MyHomePage> {
  int _counter = 0;

  static MethodChannel channel = const MethodChannel('comp_region');
  static final GlobalKey _widgetKey = GlobalKey();

  static List<String>? _groups;

  void _setCompRegions() async {
    final rb = _widgetKey.currentContext?.findRenderObject() as RenderBox?;

    if (rb == null) {
      setState(() {});
      return;
    }

    final size = rb.size;
    final offset = rb.localToGlobal(Offset.zero);

    _groups = (await channel.invokeMethod<List<dynamic>>('mask', {
      'clear': _groups ?? [],
      'groups': [
        {
          'type': 'input',
          'regions': [
            {
              'x': offset.dx.round(),
              'y': offset.dy.round(),
              'width': size.width.round(),
              'height': size.height.round(),
            },
            {
              'x': offset.dx.round(),
              'y': offset.dy.round(),
              'width': size.width.round(),
              'height': size.height.round(),
            },
            {
              'x': offset.dx.round(),
              'y': offset.dy.round(),
              'width': size.width.round(),
              'height': size.height.round(),
            }
          ]
        },
        {
          'type': 'opaque',
          'regions': [
            {
              'x': offset.dx.round(),
              'y': offset.dy.round(),
              'width': size.width.round(),
              'height': size.height.round(),
            }
          ]
        },
      ]
    }))!
        .map((o) => o as String)
        .toList();
  }

  void _increment() {
    setState(() {
      _counter++;
    });
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text(widget.title),
      ),
      body: Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: <Widget>[
            const Text(
              'You have pushed the button this many times:',
            ),
            Text(
              '$_counter',
              style: Theme.of(context).textTheme.headline4,
            ),
          ],
        ),
      ),
      floatingActionButton: Builder(
        builder: (context) {
          WidgetsBinding.instance.addPostFrameCallback((_) {
            _setCompRegions();
          });
          return FloatingActionButton(
            key: _widgetKey,
            onPressed: _increment,
            tooltip: 'Increment',
            child: const Icon(Icons.add),
          );
        },
      ),
    );
  }
}