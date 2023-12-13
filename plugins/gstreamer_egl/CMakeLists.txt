pkg_check_modules(GST IMPORTED_TARGET REQUIRED gstreamer-1.0>=1.4)
pkg_check_modules(GST_VIDEO IMPORTED_TARGET REQUIRED gstreamer-video-1.0>=1.4)
pkg_check_modules(AVFORMAT IMPORTED_TARGET libavformat<59.16.100)
if (AVFORMAT_FOUND)
    message(STATUS "AVFORMAT without Const FOUND")
else ()
    pkg_check_modules(AVFORMAT IMPORTED_TARGET REQUIRED libavformat>=59.16.100)
    message(STATUS "AVFORMAT with Const FOUND")
    add_definitions(-DAVFORMAT_WITH_CONST)
endif ()

add_library(plugin_gstreamer_egl STATIC gstreamer_egl.cc gstreamer_egl.h)
target_link_libraries(plugin_gstreamer_egl PUBLIC
        platform_homescreen
        flutter_common
        PkgConfig::AVFORMAT
        PkgConfig::GST
        PkgConfig::GST_VIDEO
)
