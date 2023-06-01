# accessibility_features plugin

## accessibility_features plugin functional test

For Accessibility Features of Flutter, please see the following.

https://docs.flutter.dev/accessibility-and-localization/accessibility

This plugin enables it and provide how to configure it via command line or json.

### Requirements

Ensure the `ivi-homescreen` build includes the following flag:

    -D CMAKE_BUILD_TYPE=Debug  -DBUILD_PLUGIN_ACCESSIBILITY=ON

### How to Run

Please install `ivi-homescreen` in PATH.

Please run `test_accessibility_features_cmdline.sh` with `--b` option and so on like the following.

```
$ ./test_accessibility_features_cmdline.sh --b=/usr/share/homescreen/bundle --f --i=1
```

Please prepare your json config file.
The json file must have the entry `"accessibility_features": %%ACCESSIBILITY%%` like the following.

```
{
   "app_id": "homescreen",
   "view":[
      {
         "bundle_path":"/usr/share/homescreen/bundle",
         "window_type": "NORMAL",
         "fullscreen": true,
         "ivi_surface_id": 1,
         "accessibility_features": %%ACCESSIBILITY%%
      }
   ]
}
```

The part `%%ACCESSIBILITY%%` will be overloaded by the script.

Please install your json config file into your machine.

Please run `test_accessibility_features_json.sh` with the path to your json config file like the following.

```
$ ./test_accessibility_features_json.sh sample.json
```
