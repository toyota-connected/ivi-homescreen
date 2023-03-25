# comp_region plugin

## Wayland Compositor Region functional test

The Compositor Region plugin enables Dart to create input and or opaque regions on the Flutter surface.  This is independent of the backend in use.

### Requirements

Ensure the `flutter-auto` build includes the following flag:

    -DBUILD_PLUGIN_COMP_REGION=ON

### Wayland Compositor - input region

The input region indicates which parts of your surface accept pointer and touch input events. You might, for example, draw a drop-shadow underneath your surface, but input events which happen in this region should be passed to the client beneath you. Or, if your window is an unusual shape, you could create an input region in that shape. For most surface types by default, your entire surface accepts input.

### Wayland Compositor - opaque region

The opaque region is a hint to the compositor as to which parts of your surface are considered opaque. Based on this information, they can optimize their rendering process. For example, if your surface is completely opaque and occludes another window beneath it, then the compositor won't waste any time on redrawing the window beneath yours. By default, this is empty, which assumes that any part of your surface might be transparent. This makes the default case the least efficient but the most correct.

## Dart data structure

### clear key
A list of strings.  Available options are:
1. input
2. opaque

If this value is empty it will not clear any regions.

### groups key
A list of region maps as specified by the `regions` key.

### regions key
A Map defining a region.  Expects the following key values
1. x
2. y
3. width
4. height

If any are left out, they will default to the value of 0.

### return value
A list of strings that might include the following values:  
1. input
2. opaque

This list response is used to set the `clear` key for next subsequent call.  Which causes regions to be cleared prior to set.


Example

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