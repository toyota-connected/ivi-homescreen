# PDF

This plugin is used with the pub.dev package `pdf`
https://pub.dev/packages/pdf

# PDFium Desktop Build

add depot_tools to your PATH

    fetch pdfium
    gclient sync --revision 7c7a6087e09e1a344984a6d0c5fbc2af36eca7ea --shallow --no-history -R -D
    gn gen --args='is_debug=false pdf_enable_v8=false pdf_enable_xfa=false pdf_is_standalone=true is_component_build=false treat_warnings_as_errors=false pdf_use_skia=true clang_use_chrome_plugins=false pdf_enable_fontations = false use_system_freetype = true use_system_libopenjpeg2 = false use_system_zlib = true use_system_libpng = true' out/Release
    ninja -C out/Release -j `nproc`
    cd out/Release
    export LD_LIBRARY_PATH=`pwd`:$LD_LIBRARY_PATH

## Functional Test Case

https://github.com/DavBfr/dart_pdf/tree/master/demo
