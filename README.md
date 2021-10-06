# ivi-homescreen

[![Build Status](https://img.shields.io/github/workflow/status/toyota-connected/ivi-homescreen/CMake/main?logoColor=red&logo=ubuntu)](https://github.com/toyota-connected/ivi-homescreen/actions) [![Total alerts](https://img.shields.io/lgtm/alerts/g/toyota-connected/ivi-homescreen.svg?logo=lgtm&logoWidth=18)](https://lgtm.com/projects/g/toyota-connected/ivi-homescreen/alerts/) [![Language grade: C/C++](https://img.shields.io/lgtm/grade/cpp/g/toyota-connected/ivi-homescreen.svg?logo=lgtm&logoWidth=18)](https://lgtm.com/projects/g/toyota-connected/ivi-homescreen/context:cpp) [![Coverity Scan Build Status](https://scan.coverity.com/projects/23813/badge.svg)](https://scan.coverity.com/projects/toyota-connected-ivi-homescreen)


IVI Homescreen for Wayland

* Strongly Typed (C++)
* Lightweight
  * Clang 11 Release Stripped = 151k
  * GCC 9.3 Release Stripped = 168k
* Source runs on Desktop and Yocto Linux
  * Ubuntu 18+
  * Fedora 33+
  * Yocto Dunfell+
* Platform Channels enabled/disabled via CMake
* OpenGL Texture Framework

# x86_64 Desktop development notes

## Ubuntu 16-18

### Logging in

Log out if logged in Login screen
Click on username field
Right-click on the gear icon below username field, and select "Ubuntu on Wayland"
Enter password and login

## Ubuntu 20+ / Fedora 33+

Defaults to Wayland, no need to do anything special

# Build steps

## Required Packages

    sudo add-apt-repository ppa:kisak/kisak-mesa
    sudo apt-get update -y
    sudo apt-get -y install libwayland-dev wayland-protocols \
    mesa-common-dev libegl1-mesa-dev libgles2-mesa-dev mesa-utils \
    libxkbcommon-dev

## GCC/libstdc++ Build

    git clone https://github.com/toyota-connected-na/ivi-homescreen.git
    mkdir build && cd build
    cmake .. -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local
    make install -j

## Clang/libc++ Build

    git clone https://github.com/toyota-connected-na/ivi-homescreen.git
    mkdir build && cd build
    CC=/usr/lib/llvm-12/bin/clang CXX=/usr/lib/llvm-12/clang++ cmake .. -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local
    make install -j

### Clang Toolchain Setup

    wget https://apt.llvm.org/llvm.sh
    chmod +x llvm.sh
    sudo ./llvm.sh 12
    sudo apt-get install -y libc++-12-dev libc++abi-12-dev libunwind-dev

## CI Example

    https://github.com/toyota-connected-na/ivi-homescreen/blob/main/.github/workflows/cmake.yml

## Debian Package

    make package -j
    sudo apt install ./ivi-homescreen-1.0.0-Release-beta-Linux-x86_64.deb

# Flutter Application
## Build

Confirm flutter/bin is in the path using: `flutter doctor -v`

    cd ~/development/my_flutter_app
    flutter channel beta
    flutter upgrade
    flutter config --enable-linux-desktop
    flutter create .
    flutter build bundle

## Install

loading path for application is: `/usr/local/share/homescreen/bundle`

This is used to leverage symlinks.  Such as:

    cd /usr/local/share/homescreen
    sudo rm -rf bundle
    sudo ln -sf ~/development/my_flutter_app/build/ bundle

Or

    sudo mkdir -p /usr/local/share/homescreen/my_flutter_app/
    sudo cp -r build/* /usr/local/share/homescreen/my_flutter_app/
    sudo ln -sf /usr/local/share/homescreen/my_flutter_app/ bundle

## Running on desktop

Copy a current icudtl.dat to /usr/local/share/flutter
Copy libflutter_engine.so to `/usr/local/lib` or use LD_LIBRARY_PATH to point downloaded engine for build:

    cd <homescreen build>
    export LD_LIBRARY_PATH=`pwd`:$LD_LIBRARY_PATH
    homescreen

## Debug

cd to flutter app folder

    flutter config --enable-linux-desktop
    flutter create .
    flutter attach --debug-port 41795 --host-vmservice-port 41795

# CMAKE dependency paths

Path prefix used to determine required files is determined at build.

For desktop `CMAKE_INSTALL_PREFIX` defaults to `/usr/local`
For target Yocto builds `CMAKE_INSTALL_PREFIX` defaults to `/usr`

# Yocto recipes

    https://github.com/jwinarske/meta-flutter
