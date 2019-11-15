# flutter-pi
A light-weight Flutter Engine Embedder for Raspberry Pi. Inspired by https://github.com/chinmaygarde/flutter_from_scratch.
Flutter-pi also runs without X11, so you don't need to boot into Raspbian Desktop & have X11 and LXDE load up; just boot into the command-line.

You can now theoretically run every flutter app you want using flutter-pi, also including extensions & plugins, just that you'd have to build the platform side of the plugins you'd like to use yourself.

_The difference between extensions and plugins is that extensions don't include any native code, they are just pure dart. Plugins (like the [connectivity plugin](https://github.com/flutter/plugins/tree/master/packages/connectivity)) include platform-specific code._

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
Then to build the asset bundle, run the following commands. I'm using flutter_gallery in this example. (note that the flutter_gallery example **does not work** with flutter-pi, since it includes plugins that have no platform-side implementation for the raspberry pi yet)
```bash
cd flutter/examples/flutter_gallery
flutter build bundle
```

After that `flutter/examples/flutter_gallery/build/flutter_assets` would be a valid path to pass as an argument to flutter-pi.

### Running your App with flutter-pi
flutter-pi doesn't support the legacy GL driver anymore. You need to activate the anholt v3d driver in raspi-config. Go to `raspi-config -> Advanced -> GL Driver` and select fake-KMS. Full-KMS is a bit buggy and doesn't work with the Raspberry Pi 7" display (or generally, any DSI display).

For some reason performance is much better when you give the VideCore only 16MB of RAM in fake-kms. I don't know why.

Also, you need to tell flutter-pi which input device to use and whether it's a touchscreen or mouse. Input devices are typically located at `/dev/input/...`. Just run `evtest` (`sudo apt install evtest`) to find out which exact path you should use. Currently only one input device is supported by flutter-pi. In the future, I will probably let flutter-pi search for an input device by itself.

Run using
```bash
./flutter-pi [flutter-pi options...] /path/to/assets/bundle/directory [flutter engine arguments...]
```

`[flutter-pi options...]` are:
- `-t /path/to/device` where `/path/to/device` is a path to a touchscreen input device (typically `/dev/input/event0` or similiar)
- `-m /path/to/device` where `/path/to/device` is a path to a mouse input device (typically `/dev/input/mouse0` or `/dev/input/event0` or similiar)

`/path/to/assets/bundle/directory` is the path of the flutter asset bundle directory (i.e. the directory containing the kernel_blob.bin)
of the flutter app you're trying to run.

`[flutter engine arguments...]` will be passed as commandline arguments to the flutter engine. You can find a list of commandline options for the flutter engine [Here](https://github.com/flutter/engine/blob/master/shell/common/switches.h).

**Note for Pi 4 users:** currently, flutter-pi will crash on the raspberry pi because it tries to open the wrong DRM device. [See Issue #13](https://github.com/ardera/flutter-pi/issues/13#issuecomment-554322089). A temporary fix is to change the path inside flutter-pi.c in the line where it says `. . . = open("/dev/dri/card0", O_RDWR);` to `/dev/dri/card1`. I'm working on an update that will enable flutter-pi to automatically select its devices.

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

[This has been fixed.](https://github.com/raspberrypi/linux/issues/3227) If you want to get the fix, you can run [rpi-update](https://github.com/hexxeh/rpi-update), which will update your firmware & operating system to the newest version.

Still, there's some delta between you touching the touchscreen and a touch event arriving at userspace. This is because of the implementation of the touch driver in the firmware & in the linux kernel. I think on average, there's a delay of 17ms. If I have enough time in the future, I'll try to build a better touchscreen driver to lower the delay.

