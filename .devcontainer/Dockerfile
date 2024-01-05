FROM debian:bookworm

RUN env DEBIAN_FRONTEND=noninteractive \
    apt-get update

RUN env DEBIAN_FRONTEND=noninteractive \
    apt-get install -y \
    git cmake ninja-build pkg-config nano clang clang-format \
    libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev libdrm-dev libgbm-dev fonts-liberation fontconfig libsystemd-dev libinput-dev libudev-dev libxkbcommon-dev libseat-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-ugly gstreamer1.0-plugins-bad gstreamer1.0-libav gstreamer1.0-alsa
