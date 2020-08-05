# flutter-pi
A light-weight Flutter Engine Embedder for Raspberry Pi. Inspired by https://github.com/chinmaygarde/flutter_from_scratch.
Flutter-pi also runs without X11, so you don't need to boot into Raspbian Desktop & have X11 and LXDE load up; just boot into the command-line.

You can now **theoretically** run every flutter app you want using flutter-pi, including apps using extensions & plugins, just that you'd have to build the platform side of the plugins you'd like to use yourself.

_The difference between extensions and plugins is that extensions don't include any native code, they are just pure dart. Plugins (like the [connectivity plugin](https://github.com/flutter/plugins/tree/master/packages/connectivity)) include platform-specific code._

## Supported Platforms
Although flutter-pi is only tested on a Rasberry Pi 4 2GB, it should work fine on other linux platforms, with the following conditions:

- support for hardware 3D acceleration. more precisely support for kernel-modesetting (KMS) and the direct rendering infrastructure (DRI) 
- CPU architecture is one of ARMv7, ARM64, x86 or x86 64bit.

This means flutter-pi won't work on a Pi Zero, Pi 1, or Pi 2. A Pi 3 works fine, even the 512MB A+ model.

If you encounter issues running flutter-pi on any of the supported platforms listed above, please report them to me and I'll fix them.

## Contents

1. **[Running your App on the Raspberry Pi](#running-your-app-on-the-raspberry-pi)**  
1.1 [Configuring your Raspberry Pi](#configuring-your-raspberry-pi)  
1.2 [Patching the App](#patching-the-app)  
1.3 [Building the Asset bundle](#building-the-asset-bundle)  
1.4 [Building the app.so](#building-the-appso-for-running-your-app-in-releaseprofile-mode)  
1.5 [Running your App with flutter-pi](#running-your-app-with-flutter-pi)  
2. **[Dependencies](#dependencies)**
3. **[Compiling flutter-pi (on the Raspberry Pi)](#compiling-flutter-pi-on-the-raspberry-pi)**  
4. **[Performance](#performance)**  
5. **[Keyboard Input](#keyboard-input)**
6. **[Touchscreen Latency](#touchscreen-latency)**  


## Running your App on the Raspberry Pi
### Configuring your Raspberry Pi
#### Switching to Console mode
flutter-pi only works when Raspbian is in console mode (no X11 or Wayland server running). To switch the Pi into console mode,
go to `raspi-config -> Boot Options -> Desktop / CLI` and select `Console` or `Console (Autologin)`.

#### Enabling the V3D driver
flutter-pi doesn't support the legacy broadcom-proprietary graphics stack anymore. You need to make sure the V3D driver in raspi-config.
Go to `raspi-config -> Advanced Options -> GL Driver` and select `GL (Fake-KMS)`.

With this driver, it's best to give the GPU as little RAM as possible in `raspi-config -> Advanced Options -> Memory Split`, which is `16MB`. This is because the V3D driver doesn't need GPU RAM anymore.

#### Fixing the GPU permissions
It seems like with newer versions of Raspbian, the `pi` user doesn't have sufficient permissions to directly access the GPU anymore. IIRC, this is because of some privilege escalation / arbitrary code execution problems of the GPU interface.

You can fix this by adding the `pi` user to the `render` group, but keep in mind that may be a security hazard:
```bash
usermod -a -G render pi
```
Then, restart your terminal session so the changes take effect. (reconnect if you're using ssh or else just reboot the Pi)

Otherwise, you'll need to run `flutter-pi` with `sudo`.

### Patching the App
First, you need to override the default target platform in your flutter app, i.e. add the following line to your _main_ method, before the _runApp_ call:
```dart
debugDefaultTargetPlatformOverride = TargetPlatform.fuchsia;
```
The _debugDefaultTargetPlatformOverride_ property is in the foundation library, so you need to import that.

Your main dart file should probably look similiar to this now:
```dart
import 'package:flutter/foundation.dart';

. . .

void main() {
  debugDefaultTargetPlatformOverride = TargetPlatform.fuchsia;
  runApp(MyApp());
}

. . .
```

### Building the Asset bundle
Then to build the asset bundle, run the following commands. You **need** to use a flutter SDK that's compatible to the engine version you're using.

I'm using `flutter_gallery` in this example. (note that the `flutter_gallery` example **does not work** with flutter-pi, since it includes plugins that have no platform-side implementation for the raspberry pi yet)
```bash
cd flutter/examples/flutter_gallery
flutter build bundle
```

After that `flutter/examples/flutter_gallery/build/flutter_assets` would be a valid path to pass as an argument to flutter-pi.

### Building the `app.so` (for running your app in Release/Profile mode)
** WIP **

### Running your App with flutter-pi
```txt
USAGE:
  flutter-pi [options] <asset bundle path> [flutter engine options]

OPTIONS:
  -i, --input <glob pattern> Appends all files matching this glob pattern to the
                             list of input (touchscreen, mouse, touchpad,
                             keyboard) devices. Brace and tilde expansion is
                             enabled.
                             Every file that matches this pattern, but is not
                             a valid touchscreen / -pad, mouse or keyboard is
                             silently ignored.
                             If no -i options are given, flutter-pi will try to
                             use all input devices assigned to udev seat0.
                             If that fails, or udev is not installed, flutter-pi
                             will fallback to using all devices matching
                             "/dev/input/event*" as inputs.
                             In most cases, there's no need to specify this
                             option.
                             Note that you need to properly escape each glob
                             pattern you use as a parameter so it isn't
                             implicitly expanded by your shell.

  --aot                      Run the app in AOT mode. The AOT snapshot
                             of the app ("app.so") must be located inside the
                             asset bundle directory.

  -o, --orientation <orientation>  Start the app in this orientation. Valid
                             for <orientation> are: portrait_up, landscape_left,
                             portrait_down, landscape_right.
                             For more information about this orientation, see
                             the flutter docs for the "DeviceOrientation"
                             enum.
                             Only one of the --orientation and --rotation
                             options can be specified.

  -r, --rotation <degrees>   Start the app with this rotation. This is just an
                             alternative, more intuitive way to specify the
                             startup orientation. The angle is in degrees and
                             clock-wise.
                             Valid values are 0, 90, 180 and 270.

  --no-text-input            Disable text input from the console.
                             This means flutter-pi won't configure the console
                             to raw/non-canonical mode.

  -h, --help                 Show this help and exit.

EXAMPLES:
  flutter-pi -i "/dev/input/event{0,1}" -i "/dev/input/event{2,3}" /home/pi/helloworld_flutterassets
  flutter-pi -i "/dev/input/mouse*" /home/pi/helloworld_flutterassets
  flutter-pi -o portrait_up ./flutter_assets
  flutter-pi -r 90 ./flutter_assets
  flutter-pi /home/pi/helloworld_flutterassets
```

`<asset bundle path>` is the path of the flutter asset bundle directory (i.e. the directory containing `kernel_blob.bin`)
of the flutter app you're trying to run.

`[flutter engine options...]` will be passed as commandline arguments to the flutter engine. You can find a list of commandline options for the flutter engine [Here](https://github.com/flutter/engine/blob/master/shell/common/switches.h).

## Dependencies
### flutter engine
flutter-pi needs `libflutter_engine.so` and `flutter_embedder.h` to compile. It also needs the flutter engine's `icudtl.dat` at runtime.
You have two options here:

- you build the engine yourself. takes a lot of time, and it most probably won't work on the first try. But once you have it set up, you have unlimited freedom on which engine version you want to use. You can find some rough guidelines [here](https://medium.com/flutter/flutter-on-raspberry-pi-mostly-from-scratch-2824c5e7dcb1).
- you can use the pre-built engine binaries I am providing [in the _engine-binaries_ branch of this project.](https://github.com/ardera/flutter-pi/tree/engine-binaries). I will only provide binaries for some engine versions though (most likely the stable ones).

### graphics libs
Additionally, flutter-pi depends on mesa's OpenGL, OpenGL ES, EGL implementation and libdrm & libgbm.
You can easily install those with `sudo apt install libgl1-mesa-dev libgles2-mesa-dev libegl-mesa0 libdrm-dev libgbm-dev`.

### fonts
The flutter engine, by default, uses the _Arial_ font. Since that doesn't come included with Raspbian, you need to install it using:
```bash
sudo apt install ttf-mscorefonts-installer fontconfig
sudo fc-cache
```
### libgpiod (for the included GPIO plugin), libsystemd, libudev
```bash
sudo apt-get install gpiod libgpiod-dev libsystemd-dev libudev-dev
```

## Compiling flutter-pi (on the Raspberry Pi)
fetch all the dependencies, clone this repo and run
```bash
cd /path/to/the/cloned/flutter-pi/directory
make
```
The _flutter-pi_ executable will then be located at this path: `/path/to/the/cloned/flutter-pi/directory/out/flutter-pi`

## Performance
Performance is actually better than I expected. With most of the apps inside the `flutter SDK -> examples -> catalog` directory I get smooth 50-60fps.

## Keyboard Input
Keyboard input is supported. **There is one important limitation though**. Text input (i.e. writing any kind of text/symbols to flutter input fields) only works when typing on the keyboard, which is attached to the terminal flutter-pi is running on. So, if you ssh into your Raspberry Pi to run flutter-pi, you have to enter text into your ssh terminal.

Raw Keyboard input (i.e. using tab to iterate through focus nodes) works with any keyboard attached to your Raspberry Pi.

converting raw key-codes to text symbols is not that easy (because of all the different keyboard layouts), so for text input flutter-pi basically uses `stdin`.

## Touchscreen Latency
Due to the way the touchscreen driver works in raspbian, there's some delta between an actual touch of the touchscreen and a touch event arriving at userspace. The touchscreen driver in the raspbian kernel actually just repeatedly polls some buffer shared with the firmware running on the VideoCore, and the videocore repeatedly polls the touchscreen. (both at 60Hz) So on average, there's a delay of 17ms (minimum 0ms, maximum 34ms). If I have enough time in the future, I'll try to build a better touchscreen driver to lower the delay.

