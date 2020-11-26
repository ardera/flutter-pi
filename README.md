## 📰 NEWS
- I created an improved touchscreen driver for Raspberry Pi 4, for lower latency & higher polling rate. See [this repo](https://github.com/ardera/raspberrypi-fast-ts) for details. The difference is noticeable, it looks a lot better and more responsive with this new driver.
- flutter-pi now requires `libxkbcommon`. Install using `sudo apt install libxkbcommon-dev`
- keyboard input works better now. You can now use any keyboard connected to the Raspberry Pi for text and raw keyboard input.

# flutter-pi
A light-weight Flutter Engine Embedder for Raspberry Pi. Inspired by https://github.com/chinmaygarde/flutter_from_scratch.
Flutter-pi also runs without X11, so you don't need to boot into Raspbian Desktop & have X11 and LXDE load up; just boot into the command-line.

You can now **theoretically** run every flutter app you want using flutter-pi, including apps using packages & plugins, just that you'd have to build the platform side of the plugins you'd like to use yourself.

_The difference between packages and plugins is that packages don't include any native code, they are just pure Dart. Plugins (like the [connectivity plugin](https://github.com/flutter/plugins/tree/master/packages/connectivity)) include platform-specific code._

## Supported Platforms
Although flutter-pi is only tested on a Rasberry Pi 4 2GB, it should work fine on other linux platforms, with the following conditions:

- support for hardware 3D acceleration. more precisely support for kernel-modesetting (KMS) and the direct rendering infrastructure (DRI) 
- CPU architecture is one of ARMv7, ARM64, x86 or x86 64bit.

This means flutter-pi won't work on a Pi Zero, Pi 1, or Pi 2. A Pi 3 works fine, even the 512MB A+ model.

If you encounter issues running flutter-pi on any of the supported platforms listed above, please report them to me and I'll fix them.

## Contents

1. **[Running your App on the Raspberry Pi](#running-your-app-on-the-raspberry-pi)**  
1.1 [Configuring your Raspberry Pi](#configuring-your-raspberry-pi)  
1.2 [Building the Asset bundle](#building-the-asset-bundle)  
1.3 [Building the `app.so` (for running your app in Release/Profile mode)](#building-the-appso-for-running-your-app-in-releaseprofile-mode)  
1.4 [Running your App with flutter-pi](#running-your-app-with-flutter-pi)  
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

With this driver, it's best to give the GPU as little RAM as possible in `raspi-config -> Advanced Options -> Memory Split`, which is `16MB`. This is because the V3D driver doesn't need GPU RAM anymore. NOTE: If you want to use the [`omxplayer_video_player`](https://pub.dev/packages/omxplayer_video_player) plugin to play back videos in flutter, you need to give the GPU some more RAM, like 64MB.

#### Fixing the GPU permissions
It seems like with newer versions of Raspbian, the `pi` user doesn't have sufficient permissions to directly access the GPU anymore. IIRC, this is because of some privilege escalation / arbitrary code execution problems of the GPU interface.

You can fix this by adding the `pi` user to the `render` group, but keep in mind that may be a security hazard:
```bash
usermod -a -G render pi
```
Then, restart your terminal session so the changes take effect. (reconnect if you're using ssh or else just reboot the Pi)

Otherwise, you'll need to run `flutter-pi` with `sudo`.

### Building the Asset bundle
Then to build the asset bundle, run the following commands on your host machine. You can't build the asset bundle on target (== your Raspberry Pi), since the flutter SDK doesn't support linux on ARM yet.

My host machine is actually running Windows. But I'm also using [WSL](https://docs.microsoft.com/de-de/windows/wsl/install-win10) to upload the binaries to the Raspberry Pi, since `rsync` is a linux tool.

**Be careful** to use a flutter SDK that's compatible to the engine version you're using.
- use flutter stable and keep it up to date. `flutter channel stable` && `flutter upgrade`
- use the latest engine binaries ([explained later](#flutter-engine)) and keep them up to date

If you encounter error messages like `Invalid kernel binary format version`, `Invalid SDK hash` or `Invalid engine hash`:
1. Make sure your flutter SDK is on `stable` and up to date and your engine binaries are up to date.
2. If you made sure that's the case and the error still happens, create a new issue for it.

I'm using [`flutter_gallery`](https://github.com/flutter/gallery) in this example. flutter_gallery is developed against flutter master. So you need to use an older version of flutter_gallery to run it with flutter stable. It seems commit [9b11f12](https://github.com/flutter/gallery/commit/9b11f127fb46cb08e70b2a7cdfe8eaa8de977d5f) is the latest one working with flutter 1.20.

```bash
git clone https://github.com/flutter/gallery.git flutter_gallery
cd flutter_gallery
git checkout 9b11f127fb46cb08e70b2a7cdfe8eaa8de977d5f
flutter build bundle
```

Then just upload the asset bundle to your Raspberry Pi. `pi@raspberrypi` is of course just an example `<username>@<hostname>` combination, your need to substitute your username and hostname there.
```bash
$ rsync -a --info=progress2 ./build/flutter_assets/ pi@raspberrypi:/home/pi/flutter_gallery_assets
```

### Building the `app.so` (for running your app in Release/Profile mode)
This is done entirely on the host machine as well.

1. First, find out the path to your flutter SDK. For me it's `C:\flutter`. (I'm on Windows)
2. Open the commandline, `cd` into your app directory.
```
git clone https://github.com/flutter/gallery.git flutter_gallery
cd flutter_gallery
git checkout 9b11f127fb46cb08e70b2a7cdfe8eaa8de977d5f
```
3. Build the kernel snapshot.
```cmd
C:\flutter\bin\cache\dart-sdk\bin\dart.exe ^
  C:\flutter\bin\cache\dart-sdk\bin\snapshots\frontend_server.dart.snapshot ^
  --sdk-root C:\flutter\bin\cache\artifacts\engine\common\flutter_patched_sdk_product ^
  --target=flutter ^
  --aot ^
  --tfa ^
  -Ddart.vm.product=true ^
  --packages .packages ^
  --output-dill build\kernel_snapshot.dill ^
  --verbose ^
  --depfile build\kernel_snapshot.d ^
  package:gallery/main.dart
```
4. Build the `app.so`. This uses the `gen_snapshot_linux_x64` executable I provide in the engine-binaries branch. It needs to be executed under linux. If you're on Windows, you need to use [WSL](https://docs.microsoft.com/de-de/windows/wsl/install-win10).
```bash
$ git clone --branch engine-binaries https://github.com/ardera/flutter-pi ~/engine-binaries
$ cd /path/to/your/app
$ ~/engine-binaries/gen_snapshot_linux_x64 \
  --causal_async_stacks \
  --deterministic \
  --snapshot_kind=app-aot-elf \
  --elf=build/app.so \
  --strip \
  --sim_use_hardfp \
  --no-use-integer-division \
  build/kernel_snapshot.dill
```
5. Upload the asset bundle and the `app.so` to your Raspberry Pi. Flutter-pi expects the `app.so` to be located inside the asset bundle directory.
```bash
$ rsync -a --info=progress2 ./build/flutter_assets/ pi@raspberrypi:/home/pi/flutter_gallery_assets
$ scp ./build/app.so pi@raspberrypi:/home/pi/flutter_gallery_assets/app.so
```
6. When starting your app, make sure you invoke flutter-pi with the `--release` flag.

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

  --release                  Run the app in release mode. The AOT snapshot
                             of the app ("app.so") must be located inside the
                             asset bundle directory.
                                                         This also requires a libflutter_engine.so that was
                                                         built with --runtime-mode=release.

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

  -d, --dimensions "width_mm,height_mm" The width & height of your display in
                             millimeters. Useful if your GPU doesn't provide
                                                         valid physical dimensions for your display.
                                                         The physical dimensions of your display are used
                                                         to calculate the flutter device-pixel-ratio, which
                                                         in turn basically "scales" the UI.

  --no-text-input            Disable text input from the console.
                             This means flutter-pi won't configure the console
                             to raw/non-canonical mode.

  -h, --help                 Show this help and exit.

EXAMPLES:
  flutter-pi -i "/dev/input/event{0,1}" -i "/dev/input/event{2,3}" /home/pi/helloworld_flutterassets
  flutter-pi -i "/dev/input/mouse*" /home/pi/helloworld_flutterassets
  flutter-pi -o portrait_up ./flutter_assets
  flutter-pi -r 90 ./flutter_assets
  flutter-pi -d "155, 86" ./flutter_assets
  flutter-pi /home/pi/helloworld_flutterassets

SEE ALSO:
  Author:  Hannes Winkler, a.k.a ardera
  Source:  https://github.com/ardera/flutter-pi
  License: MIT

  For instructions on how to build an asset bundle or an AOT snapshot
    of your app, please see the linked git repository.
  For a list of options you can pass to the flutter engine, look here:
    https://github.com/flutter/engine/blob/master/shell/common/switches.h
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
### libgpiod (for the included GPIO plugin), libsystemd, libinput, libudev
```bash
sudo apt-get install gpiod libgpiod-dev libsystemd-dev libinput-dev libudev-dev libxkbcommon-dev
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

## Touchscreen Latency
Due to the way the touchscreen driver works in raspbian, there's some delta between an actual touch of the touchscreen and a touch event arriving at userspace. The touchscreen driver in the raspbian kernel actually just repeatedly polls some buffer shared with the firmware running on the VideoCore, and the videocore repeatedly polls the touchscreen. (both at 60Hz) So on average, there's a delay of 17ms (minimum 0ms, maximum 34ms). Actually, the firmware is polling correctly at ~60Hz, but the linux driver is not because there's a bug. The linux side actually polls at 25Hz, which makes touch applications look terrible. (When you drag something in a touch application, but the application only gets new touch data at 25Hz, it'll look like the application itself is _redrawing_ at 25Hz, making it look very laggy) The github issue for this raspberry pi kernel bug is [here](https://github.com/raspberrypi/linux/issues/3777). Leave a like on the issue if you'd like to see this fixed in the kernel.

This is why I created my own (userspace) touchscreen driver, for improved latency & polling rate. See [this repo](https://github.com/ardera/raspberrypi-fast-ts) for details. The driver is very easy to use and the difference is noticeable, flutter apps look and feel a lot better with this driver.
