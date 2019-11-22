# flutter-pi
A light-weight Flutter Engine Embedder for Raspberry Pi. Inspired by https://github.com/chinmaygarde/flutter_from_scratch.
Flutter-pi also runs without X11, so you don't need to boot into Raspbian Desktop & have X11 and LXDE load up; just boot into the command-line.

You can now theoretically run every flutter app you want using flutter-pi, also including extensions & plugins, just that you'd have to build the platform side of the plugins you'd like to use yourself.

_The difference between extensions and plugins is that extensions don't include any native code, they are just pure dart. Plugins (like the [connectivity plugin](https://github.com/flutter/plugins/tree/master/packages/connectivity)) include platform-specific code._

**Note:** flutter-pi should also work just fine on other 32-bit platforms, if they have Kernel-Modesetting and Direct-Rendering-Infrastructure support. (64-bit support is trivial, just haven't got around to implement it yet)

## Contents

1. **[Running your App on the Raspberry Pi](#running-your-app-on-the-raspberry-pi)**  
1.1 [Patching the App](#patching-the-app)  
1.2 [Building the Asset bundle](building-the-asset-bundle)  
1.3 [Running your App with flutter-pi](running-your-app-with-flutter-pi)  
2. **[Dependencies](#dependencies)**  
2.1 [flutter engine](#flutter-engine)  
2.2 [graphics libs](#graphics-libs)  
2.3 [fonts](#fonts)  
3. **[Compiling flutter-pi (on the Raspberry Pi)](#compiling-flutter-pi-on-the-raspberry-pi)**  
4. **[Performance](#performance)**  
5. **[Touchscreen Bug](#touchscreen-bug)**  


## Running your App on the Raspberry Pi
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

### Running your App with flutter-pi
flutter-pi doesn't support the legacy broadcom-proprietary graphics stack anymore. You need to activate the V3D / VC4-V3D driver in raspi-config. Go to `raspi-config -> Advanced -> GL Driver` and select fake-KMS. Full-KMS is a bit buggy and doesn't work with the Raspberry Pi 7" display (or generally, any DSI display).

For Raspberry Pi's older than the 4B Model, it's best to give the GPU as little RAM as possible in `raspi-config` (16MB), since the V3D / VC4-V3D driver doesn't need GPU RAM anymore. The Pi 4 automatically adjusts the memory split at runtime.

```txt
USAGE:
  flutter-pi [options] <asset bundle path> [flutter engine options...]

OPTIONS:
  -i <glob pattern>   Appends all files matching this glob pattern
                      to the list of input (touchscreen, mouse, touchpad)
                      devices. Brace and tilde expansion is enabled.
                      Every file that matches this pattern, but is not
                      a valid touchscreen / -pad or mouse is silently
                      ignored.
                        If no -i options are given, all files matching
                      "/dev/input/event*" will be used as inputs.
                      This should be what you want in most cases.
                        Note that you need to properly escape each glob pattern
                      you use as a parameter so it isn't implicitly expanded
                      by your shell.

  -h                  Show this help and exit.

EXAMPLES:
  flutter-pi -i "/dev/input/event{0,1}" -i "/dev/input/event{2,3}" /home/helloworld_flutterassets
  flutter-pi -i "/dev/input/mouse*" /home/pi/helloworld_flutterassets
  flutter-pi /home/pi/helloworld_flutterassets
```

Also, `<asset bundle path>` is the path of the flutter asset bundle directory (i.e. the directory containing `kernel_blob.bin`)
of the flutter app you're trying to run.

`[flutter engine options...]` will be passed as commandline arguments to the flutter engine. You can find a list of commandline options for the flutter engine [Here](https://github.com/flutter/engine/blob/master/shell/common/switches.h).

It seems that sometimes, when running on the Pi 4, you need to run flutter-pi as root. At other times, it works fine without root. Don't know why that may be. If root is needed and flutter-pi is run without root, mesa will select _llvmpipe_ as the driver, and flutter-pi will issue a warning (and probably crash right after that).

## Dependencies
### flutter engine
flutter-pi needs `libflutter_engine.so` and `flutter_embedder.h` to compile. It also needs the flutter engine's `icudtl.dat` at runtime.
You have to options here:

- you build the engine yourself. takes a lot of time, and it most probably won't work on the first try. But once you have it set up, you have unlimited freedom on which engine version you want to use. You can find some rough guidelines [here](https://medium.com/flutter/flutter-on-raspberry-pi-mostly-from-scratch-2824c5e7dcb1). [Andrew jones](https://github.com/andyjjones28) is working on some more detailed instructions.
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

## Compiling flutter-pi (on the Raspberry Pi)
fetch all the dependencies, clone this repo and run
```bash
cd /path/to/the/cloned/flutter-pi/directory
make
```
The _flutter-pi_ executable will then be located at this path: `/path/to/the/cloned/flutter-pi/directory/out/flutter-pi`

## Performance
Performance is actually better than I expected. With most of the apps inside the `flutter SDK -> examples -> catalog` directory I get smooth 50-60fps.

## Touchscreen Bug
~~If you use the official 7 inch touchscreen, performance will feel much worse while dragging something. This seems to be some bug in the touchscreen driver. The embedder / userspace only gets around 25 touch events a second, meaning that while dragging something (like in tabbed_app_bar.dart), the position of the object being dragged is only updated 25 times a second. This results in the app looking like it runs at 25fps. The touchscreen could do up to 100 touch updates a second though.~~

[This has been fixed.](https://github.com/raspberrypi/linux/issues/3227) Still, there's some delta between you touching the touchscreen and a touch event arriving at userspace. This is because of the implementation of the touch driver in the firmware & in the linux kernel. I think on average, there's a delay of 17ms. If I have enough time in the future, I'll try to build a better touchscreen driver to lower the delay.

