#!/usr/bin/env bash

sudo apt-get install -y --no-install-recommends \
    git cmake pkg-config ninja-build clang clang-tools \
    libgl-dev libgles-dev libegl-dev libvulkan-dev libdrm-dev libgbm-dev libsystemd-dev libinput-dev libudev-dev libxkbcommon-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev

$WRAPPER cmake \
    -S . -B build \
    -GNinja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN=ON \
    -DBUILD_GSTREAMER_AUDIO_PLAYER_PLUGIN=ON \
    -DENABLE_VULKAN=ON \
    -DENABLE_SESSION_SWITCHING=ON

$WRAPPER cmake --build build
