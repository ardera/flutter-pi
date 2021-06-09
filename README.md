## 📰 NEWS
- The new latest flutter gallery commit for flutter 2.2 is `633be8a`
- There's now a `#custom-embedders` channel on the [flutter discord](https://github.com/flutter/flutter/wiki/Chat) which you can use if you have any questions regarding flutter-pi or generally, anything related to embedding the engine for which you don't want to open issue about or write an email.

# flutter-pi
A light-weight Flutter Engine Embedder for Raspberry Pi. Inspired by https://github.com/chinmaygarde/flutter_from_scratch.
Flutter-pi also runs without X11, so you don't need to boot into Raspbian Desktop & have X11 and LXDE load up; just boot into the command-line.

You can now **theoretically** run every flutter app you want using flutter-pi, including apps using packages & plugins, just that you'd have to build the platform side of the plugins you'd like to use yourself.

_The difference between packages and plugins is that packages don't include any native code, they are just pure Dart. Plugins (like the [connectivity plugin](https://github.com/flutter/plugins/tree/master/packages/connectivity)) include platform-specific code._

## 🖥️ Supported Platforms
Although flutter-pi is only tested on a Rasberry Pi 4 2GB, it should work fine on other linux platforms, with the following conditions:

- support for hardware 3D acceleration. more precisely support for kernel-modesetting (KMS) and the direct rendering infrastructure (DRI) 
- CPU architecture is one of ARMv7, ARMv8, x86 or x86 64bit.

This means flutter-pi won't work on a Pi Zero or Pi 1. A Pi 3 works fine, even the 512MB A+ model, and a Pi 2 should work fine too.

If you encounter issues running flutter-pi on any of the supported platforms listed above, please report them to me and I'll fix them.

## 📑 Contents

1. **[Building flutter-pi on the Raspberry Pi](#-building-flutter-pi-on-the-raspberry-pi)**  
1.1 [Dependencies](#dependencies)  
1.2 [Compiling](#compiling)  
2. **[Running your App on the Raspberry Pi](#-running-your-app-on-the-raspberry-pi)**  
2.1 [Configuring your Raspberry Pi](#configuring-your-raspberry-pi)  
2.2 [Building the Asset bundle](#building-the-asset-bundle)  
2.3 [Building the `app.so` (for running your app in Release/Profile mode)](#building-the-appso-for-running-your-app-in-releaseprofile-mode)  
2.4 [Running your App with flutter-pi](#running-your-app-with-flutter-pi)  
3. **[Performance](#-performance)**  
3.1 [Graphics Performance](#graphics-performance)  
3.2 [Touchscreen latency](#touchscreen-latency)  

## 🛠 Building flutter-pi on the Raspberry Pi
- If you want to update flutter-pi, you check out the latest commit using `git pull && git checkout origin/master` and continue with [compiling](#compiling), step 2.

### Dependencies
1. Install the flutter engine binaries using the instructions in the [in the _flutter-engine-binaries-for-arm_ repo.](https://github.com/ardera/flutter-engine-binaries-for-arm).
    <details>
      <summary>More Info</summary>

      flutter-pi needs flutters `flutter_embedder.h` to compile and `icudtl.dat` at runtime. It also needs `libflutter_engine.so.release` at runtime when invoked with the `--release` flag and `libflutter_engine.so.debug` when invoked without.
      You actually have two options here:

      - you build the engine yourself. takes a lot of time, and it most probably won't work on the first try. But once you have it set up, you have unlimited freedom on which engine version you want to use. You can find some rough guidelines [here](https://medium.com/flutter/flutter-on-raspberry-pi-mostly-from-scratch-2824c5e7dcb1).
      - you can use the pre-built engine binaries I am providing [in the _flutter-engine-binaries-for-arm_ repo.](https://github.com/ardera/flutter-engine-binaries-for-arm). I will only provide binaries for some engine versions though (most likely the stable ones).

    </details>

2. Install cmake, graphics, system libraries and fonts:
    ```bash
    sudo apt install cmake libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev libdrm-dev libgbm-dev ttf-mscorefonts-installer fontconfig libsystemd-dev libinput-dev libudev-dev  libxkbcommon-dev
    ```
    <details>
      <summary>More Info</summary>
      
      - flutter-pi needs the mesa OpenGL ES and EGL implementation and libdrm & libgbm. It may work with non-mesa implementations too, but that's untested.
      - The flutter engine depends on the _Arial_ font. Since that doesn't come included with Raspbian, you need to install it.
      - `libsystemd` is not systemd, it's just an utility library. It provides the event loop and dbus support for flutter-pi.
      - `libinput-dev`, `libudev-dev` and `libxkbcommon-dev` are needed for (touch, mouse, raw keyboard and text) input support.
      - `libudev-dev` is required, but actual udev is not. Flutter-pi will just open all `event` devices inside `/dev/input` (unless overwritten using `-i`) if udev is not present.
      - `gpiod` and `libgpiod-dev` where required in the past, but aren't anymore since the `flutter_gpiod` plugin will directly access the kernel interface.
    </details>
    
3. Update the system fonts.
    ```bash
    sudo fc-cache
    ```

### Compiling
1. Clone flutter-pi and cd into the cloned directory:
    ```bash
    git clone https://github.com/ardera/flutter-pi
    cd flutter-pi
    ```
2. Compile:
    ```bash
    mkdir build && cd build
    cmake ..
    make -j`nproc`
    ```
3. Install:
    ```bash
    sudo make install
    ```

## 🚀 Running your App on the Raspberry Pi
### Configuring your Raspberry Pi
1. Open raspi-config:
    ```bash
    sudo raspi-config
    ```
    
2. Switch to console mode:
   `System Options -> Boot / Auto Login` and select `Console` or `Console (Autologin)`.

3. Enable the V3D graphics driver
   `Advanced Options -> GL Driver -> GL (Fake KMS)`

4. Configure the GPU memory
   `Performance Options -> GPU Memory` and enter `64`.

5. Leave `raspi-config`.

6. Give the `pi` permission to use 3D acceleration. (**NOTE:** potential security hazard. If you don't want to do this, launch `flutter-pi` using `sudo` instead.)
    ```bash
    usermod -a -G render pi
    ```

5. Finish and reboot.

<details>
  <summary>More information</summary>
  
  - flutter-pi requires that no other process, like a X11- or wayland-server, is using the video output. So to disable the desktop environment, we boot into console instead.
  - The old broadcom-proprietary GL driver was bugged and not working with flutter, so we have to use the Fake KMS driver.
  - Actually, you can also configure 16MB of GPU memory if you want to. 64MB are needed when you want to use the [`omxplayer_video_player`](https://pub.dev/packages/omxplayer_video_player) plugin.
  - `pi` isn't allowed to directly access the GPU because IIRC this has some privilege escalation bugs. Raspberry Pi has quite a lot of system-critical, not graphics-related stuff running on the GPU. I read somewhere it's easily possible to gain control of the GPU by writing malicious shaders. From there you can gain control of the CPU and thus the linux kernel. So basically the `pi` user could escalate privileges and become `root` just by directly accessing the GPU. But maybe this has already been fixed, I'm not sure.
</details>

### Building the Asset bundle
- The asset bundle must be built on your development machine. Note that you can't use a Raspberry Pi as your development machine.

1. Make sure you've installed the flutter SDK. **You must** use a flutter SDK that's compatible to the installed engine binaries.
   - for the flutter SDK, use flutter stable and keep it up to date.  
   - always use the latest available [engine binaries](https://github.com/ardera/flutter-engine-binaries-for-arm)  
   
   If you encounter error messages like `Invalid kernel binary format version`, `Invalid SDK hash` or `Invalid engine hash`:
   1. Make sure your flutter SDK is on `stable` and up to date and your engine binaries are up to date.
   2. If you made sure that's the case and the error still happens, create a new issue.
   
2. Open terminal or commandline and `cd` into your app directory.

3. `flutter build bundle`

4. Deploy the asset bundle to the Raspberry Pi using `rsync` or `scp`.
   - Using `rsync` (available on linux and macOS or on Windows when using [WSL](https://docs.microsoft.com/de-de/windows/wsl/install-win10))
       ```bash
       rsync -a --info=progress2 ./build/flutter_assets/ pi@raspberrypi:/home/pi/my_apps_flutter_assets
       ```
   - Using `scp` (available on linux, macOS and Windows)
       ```bash
       scp -r ./build/flutter_assets/ pi@raspberrypi:/home/pi/my_apps_flutter_assets
       ```
       
#### Example
1. We'll build the asset bundle for `flutter_gallery` and deploy it using `rsync` in this example.
```bash
git clone https://github.com/flutter/gallery.git flutter_gallery
cd flutter_gallery
git checkout 633be8a
flutter build bundle
rsync -a ./build/flutter_assets/ pi@raspberrypi:/home/pi/flutter_gallery/
```
3. Done. You can now run this app in debug-mode using `flutter-pi /home/pi/flutter_gallery`.

<details>
  <summary>More information</summary>
    
  - flutter_gallery is developed against flutter master. `633be8aa13799bf1215d03a155132025f42c7d07` is currently the latest flutter gallery
    commit working with flutter stable.
</details>

### Building the `app.so` (for running your app in Release/Profile mode)
- This is done entirely on your development machine as well.

1. Find out the path to your flutter SDK. For me it's `C:\flutter`. (I'm on Windows)
2. Open terminal or commandline and `cd` into your app directory.
3. Build the asset bundle.
   ```
   flutter build bundle
   ```
4. Build the kernel snapshot. (Replace `my_app_name` with the name of your app)
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
      package:my_app_name/main.dart
    ```

5. Fetch the latest `gen_snapshot_linux_x64_release` I provide in the [engine binaries repo](https://github.com/ardera/flutter-engine-binaries-for-arm).
6. The following steps must be executed on a linux x64 machine. If you're on windows, you can use [WSL](https://docs.microsoft.com/de-de/windows/wsl/install-win10). If you're on macOS, you can use a linux VM.
7. Build the `app.so`. If you're building for _arm64_, you need to omit the `--sim-use-hardfp` flag.
    ```bash
    gen_snapshot_linux_x64_release \
      --deterministic \
      --snapshot_kind=app-aot-elf \
      --elf=build/flutter_assets/app.so \
      --strip \
      --sim-use-hardfp \
      build/kernel_snapshot.dill
    ```
8. Now you can switch to your normal OS again.
9. Upload the asset bundle and the `app.so` to your Raspberry Pi.
    ```bash
    rsync -a --info=progress2 ./build/flutter_assets/ pi@raspberrypi:/home/pi/my_app
    ```
    or
    ```
    scp -r ./build/flutter_assets/ pi@raspberrypi:/home/pi/my_app
    ```
10. You can now launch the app in release mode using `flutter-pi --release /home/pi/my_app`

#### Complete example on Windows
1. We'll build the asset bundle for `flutter_gallery` and deploy it using `rsync` in this example.
    ```bash
    git clone https://github.com/flutter/gallery.git flutter_gallery
    git clone --depth 1 https://github.com/ardera/flutter-engine-binaries-for-arm.git engine-binaries
    cd flutter_gallery
    git checkout 633be8a
    flutter build bundle
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
    wsl
    ../engine-binaries/arm/gen_snapshot_linux_x64_release \
      --deterministic \
      --snapshot_kind=app-aot-elf \
      --elf=build/flutter_assets/app.so \
      --strip \
      --sim-use-hardfp \
      build/kernel_snapshot.dill
    rsync -a --info=progress2 ./build/flutter_assets/ pi@raspberrypi:/home/pi/flutter_gallery/
    exit
    ```
3. Done. You can now run this app in release mode using `flutter-pi --release /home/pi/flutter_gallery`.

### Running your App with flutter-pi
```txt
USAGE:
  flutter-pi [options] <asset bundle path> [flutter engine options]

OPTIONS:
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

  -h, --help                 Show this help and exit.

EXAMPLES:
  flutter-pi ~/hello_world_app
  flutter-pi --release ~/hello_world_app
  flutter-pi -o portrait_up ./my_app
  flutter-pi -r 90 ./my_app
  flutter-pi -d "155, 86" ./my_app

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

## 📊 Performance
### Graphics Performance
Graphics performance is actually pretty good. With most of the apps inside the `flutter SDK -> examples -> catalog` directory I get smooth 50-60fps on the Pi 4 2GB and Pi 3 A+.

### Touchscreen Latency
Due to the way the touchscreen driver works in raspbian, there's some delta between an actual touch of the touchscreen and a touch event arriving at userspace. The touchscreen driver in the raspbian kernel actually just repeatedly polls some buffer shared with the firmware running on the VideoCore, and the videocore repeatedly polls the touchscreen. (both at 60Hz) So on average, there's a delay of 17ms (minimum 0ms, maximum 34ms). Actually, the firmware is polling correctly at ~60Hz, but the linux driver is not because there's a bug. The linux side actually polls at 25Hz, which makes touch applications look terrible. (When you drag something in a touch application, but the application only gets new touch data at 25Hz, it'll look like the application itself is _redrawing_ at 25Hz, making it look very laggy) The github issue for this raspberry pi kernel bug is [here](https://github.com/raspberrypi/linux/issues/3777). Leave a like on the issue if you'd like to see this fixed in the kernel.

This is why I created my own (userspace) touchscreen driver, for improved latency & polling rate. See [this repo](https://github.com/ardera/raspberrypi-fast-ts) for details. The driver is very easy to use and the difference is noticeable, flutter apps look and feel a lot better with this driver.
