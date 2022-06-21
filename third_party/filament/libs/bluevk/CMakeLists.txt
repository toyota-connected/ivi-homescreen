cmake_minimum_required(VERSION 3.19)
project(bluevk)

set(TARGET              bluevk)
set(PUBLIC_HDR_DIR      include)

file(GLOB_RECURSE PUBLIC_HDRS ${PUBLIC_HDR_DIR}/${TARGET}/*.h)

set(SRCS src/BlueVK.cpp)

# OS specific sources
if (WIN32)
    set(SRCS ${SRCS} src/BlueVKWindows.cpp)
elseif (APPLE)
    set(SRCS ${SRCS} src/BlueVKDarwin.cpp)
elseif (LINUX OR ANDROID)
    set(SRCS ${SRCS} src/BlueVKLinuxAndroid.cpp)
else()
    message(FATAL_ERROR "Platform not supported. BlueVK supports Linux, Android, MacOS X, and Windows.")
endif()

include_directories(${PUBLIC_HDR_DIR})

add_library(${TARGET} STATIC ${PUBLIC_HDRS} ${SRCS})

target_include_directories(${TARGET} PUBLIC ${PUBLIC_HDR_DIR})

install(TARGETS ${TARGET} ARCHIVE DESTINATION lib/${DIST_DIR})
