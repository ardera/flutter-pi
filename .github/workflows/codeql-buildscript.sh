#!/usr/bin/env bash

sudo apt install -y cmake libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev libdrm-dev libgbm-dev ttf-mscorefonts-installer fontconfig libsystemd-dev libinput-dev libudev-dev  libxkbcommon-dev
mkdir build && cd build
cmake ..
make -j`nproc`
