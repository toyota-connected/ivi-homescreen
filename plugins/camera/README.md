# Camera Plugin Development

This file contains the development plan for the Camera Plugin project.

## Current Progress

The Camera Plugin can currently handle `availableCameras`, `create`, and `initialize`.

## Future Development

The following aspects are still under development:

- Building out the texture registrar: We're still figuring out how to best implement this part of the plugin.
- Session and FrameSink classes: These classes need to be created to handle different parts of the camera's operation.

## libcamera logging output

To set logging to show only errors:

    export LIBCAMERA_LOG_LEVELS=*:ERROR

## Flutter Example

You can refer to this **[Flutter Example](https://github.com/flutter/packages/tree/main/packages/camera/camera/example)
** for more detailed usage of camera plugin.