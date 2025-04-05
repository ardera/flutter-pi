## 📰 NEWS
- RISC-V is now supported as as both a host and target platform by flutterpi_tool!
- Added a (not complete) sentry plugin, see: https://github.com/ardera/flutter-pi/wiki/Sentry-Support

# flutter-pi
A light-weight Flutter Engine Embedder for Raspberry Pi. Inspired by https://github.com/chinmaygarde/flutter_from_scratch.
Flutter-pi also runs without X11, so you don't need to boot into Raspbian Desktop & have X11 and LXDE load up; just boot into the command-line.

You can now **theoretically** run every flutter app you want using flutter-pi, including apps using packages & plugins, just that you'd have to build the platform side of the plugins you'd like to use yourself.

_The difference between packages and plugins is that packages don't include any native code, they are just pure Dart. Plugins (like the [shared_preferences plugin](https://github.com/flutter/plugins/tree/main/packages/shared_preferences)) include platform-specific code._

## 🖥️ Supported Platforms
Flutter-Pi is working on the following Boards & SoCs:
- Raspberry Pi 2, 3, 4, 5 and Zero 2 (even the 512MB models)
- Texas Instruments AM62, NXP i.MX8 (using yocto & [meta-flutter](https://github.com/meta-flutter/meta-flutter) or [buildroot](https://buildroot.org/))
- Banana Pi BPI-F3

It should work fine on other linux platforms, granted that the following conditions are satisfied:
- Support for hardware 3D acceleration. More precisely support for kernel-modesetting (KMS) and the direct rendering infrastructure (DRI).
  - E.g. the [NXP i.MX6](https://www.nxp.com/products/processors-and-microcontrollers/arm-processors/i-mx-applications-processors/i-mx-6-processors:IMX6X_SERIES) will not work by default when using the proprietary Vivante driver, however it's very easy to just use [etnaviv](https://github.com/etnaviv/etna_viv) instead.
- CPU architecture is ARMv7, ARMv8, x86 64bit or RISC-V. (Requirement of the Flutter Engine)
  - This means flutter-pi won't work on a Pi Zero (only the first one) or Raspberry Pi 1, which have an ARMv6 core.

If you encounter issues running flutter-pi on any platform that should be working, please report them to me and I'll fix them!

## 📑 Contents

1. **[Building flutter-pi on the Raspberry Pi](#-building-flutter-pi-on-the-raspberry-pi)**  
1.1 [Dependencies](#dependencies)  
1.2 [Compiling](#compiling)  
2. **[Running your App on the Raspberry Pi](#-running-your-app-on-the-raspberry-pi)**  
2.1 [Configuring your Raspberry Pi](#configuring-your-raspberry-pi)  
2.2 [Building the App](#building-the-app-new-method-linux-only)  
2.3 [Running your App with flutter-pi](#running-your-app-with-flutter-pi)  
2.4 [gstreamer video player](#gstreamer-video-player)  
2.5 [audioplayers](#audioplayers)
3. **[Performance](#-performance)**  
3.1 [Graphics Performance](#graphics-performance)  
3.2 [Touchscreen latency](#touchscreen-latency)
4. **[Useful Dart Packages](#-useful-dart-packages)**
5. **[Discord](#-discord)**

## 🛠 Building flutter-pi on the Raspberry Pi
- If you want to update flutter-pi, you check out the latest commit using `git pull && git checkout origin/master` and continue with [compiling](#compiling), step 2.

### Dependencies

1. Install cmake, graphics & system libraries and fonts:
    ```shell
    sudo apt install cmake libgles-dev libegl-dev libdrm-dev libgbm-dev libsystemd-dev libinput-dev libudev-dev libxkbcommon-dev fontconfig fonts-liberation
    ```

    If you want to use the [gstreamer video player](#gstreamer-video-player), install these too:
    ```shell
    sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-ugly gstreamer1.0-plugins-bad gstreamer1.0-libav gstreamer1.0-alsa
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
    
4. Update the system fonts.
    ```bash
    sudo fc-cache
    ```

### Compiling
1. Clone flutter-pi and cd into the cloned directory:
    ```bash
    git clone --recursive https://github.com/ardera/flutter-pi && cd flutter-pi
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

## 🚀 Running your App on the Device
1. Make sure you disabled your desktop environment:
    ```bash
    sudo systemctl set-default multi-user.target
    ```
    (You can switch back to graphical using `sudo systemctl set-default graphical.target`

2. Give the `pi` permission to use 3D acceleration. (**NOTE:** potential security hazard. If you don't want to do this, launch `flutter-pi` using `sudo` instead.)
    ```bash
    usermod -a -G render pi
    ```
3. Finish and reboot.

<details>
  <summary>More information</summary>
  
  - flutter-pi requires that no other process, like a X11- or wayland-server, is using the video output. So to disable the desktop environment, we boot into console instead.
  - The old broadcom-proprietary GL driver was bugged and not working with flutter, so we have to use the Fake KMS driver.
  - Actually, you can also configure 16MB of GPU memory if you want to. 64MB are needed when you want to use the [`omxplayer_video_player`](https://pub.dev/packages/omxplayer_video_player) plugin.
  - `pi` isn't allowed to directly access the GPU because IIRC this has some privilege escalation bugs. Raspberry Pi has quite a lot of system-critical, not graphics-related stuff running on the GPU. I read somewhere it's easily possible to gain control of the GPU by writing malicious shaders. From there you can gain control of the CPU and thus the linux kernel. So basically the `pi` user could escalate privileges and become `root` just by directly accessing the GPU. But maybe this has already been fixed, I'm not sure.
</details>

### Running the app
The app can be built anywhere you want, e.g. Mac, Windows and Linux (arm32, arm64, x64, risc-v).

_One-time setup:_
1. Make sure you've installed the Flutter SDK.
2. Install the [flutterpi_tool](https://pub.dev/packages/flutterpi_tool):
   Run `flutter pub global activate flutterpi_tool` (One time only)
3. If running `flutterpi_tool` directly doesn't work, follow https://dart.dev/tools/pub/cmd/pub-global#running-a-script-from-your-path
   to add the dart global bin directory to your path.
4. If there are dependency conflicts after upgrading the Flutter SDK, first: try deactivating and then activating flutterpi_tool again:
   ```
   flutter pub global deactivate flutterpi_tool
   flutter pub global activate flutterpi_tool
   ```
   If the version problems persist, it might be flutterpi_tool doesn't support the new Flutter SDK yet.
   In that case, you can try going back to the next oldest stable SDK version. (e.g. 3.27 instead of 3.29)
   Same applies if there's an `engine artifacts for ... are not yet available` error.

_Device Configuration_
1. Configure your target device using `flutterpi_tool`, e.g. if you ssh into the device with `ssh pi@my-pi`, run: `flutterpi_tool devices add pi@my-pi`.
2. If there's any things that still need to be configured on the target device, flutterpi_tool should warn you about those.

_Running an app_
1. Open terminal or commandline and `cd` into your app directory.
2. Run the app on the target using `flutterpi_tool run -d my-pi`

_Example:_
1. We'll build the asset bundle for `flutter_gallery` and run it on a Raspberry Pi 4 called `my-pi` in this example.
```bash
git clone https://github.com/flutter/gallery.git flutter_gallery && cd flutter_gallery
flutterpi_tool devices add pi@my-pi
flutterpi_tool run -d my-pi
```

2. On Raspberry Pi, run `` to install the runtime requirement of flutter_gallery. (otherwise it may [throw exception](https://github.com/flutter/gallery/issues/979#issuecomment-1693361972))

### Running your App with flutter-pi
```txt
pi@hpi4:~ $ flutter-pi --help
flutter-pi - run flutter apps on your Raspberry Pi.

USAGE:
  flutter-pi [options] <bundle path> [flutter engine options]

OPTIONS:
  --release                  Run the app in release mode. The AOT snapshot
                             of the app must be located inside the bundle directory.
                             This also requires a libflutter_engine.so that was
                             built with --runtime-mode=release.

  --profile                  Run the app in profile mode. The AOT snapshot
                             of the app must be located inside the bundle directory.
                             This also requires a libflutter_engine.so that was
                             built with --runtime-mode=profile.

  --vulkan                   Use vulkan for rendering.

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

  --pixelformat <format>     Selects the pixel format to use for the framebuffers.
                             If this is not specified, a good pixel format will
                             be selected automatically.
                             Available pixel formats: RGB565, ARGB4444, XRGB4444, ARGB1555, XRGB1555, ARGB8888, XRGB8888, BGRA8888, BGRX8888, RGBA8888, RGBX8888, 
  --videomode widthxheight
  --videomode widthxheight@hz  Uses an output videomode that satisfies the argument.
                             If no hz value is given, the highest possible refreshrate
                             will be used.

  --dummy-display            Simulate a display. Useful for running apps
                             without a display attached.
  --dummy-display-size "width,height" The width & height of the dummy display
                             in pixels.

  -h, --help                 Show this help and exit.

EXAMPLES:
  flutter-pi ~/hello_world_app
  flutter-pi --release ~/hello_world_app
  flutter-pi -o portrait_up ./my_app
  flutter-pi -r 90 ./my_app
  flutter-pi -d "155, 86" ./my_app
  flutter-pi --videomode 1920x1080 ./my_app
  flutter-pi --videomode 1280x720@60 ./my_app

SEE ALSO:
  Author:  Hannes Winkler, a.k.a ardera
  Source:  https://github.com/ardera/flutter-pi
  License: MIT

  For instructions on how to build an asset bundle or an AOT snapshot
    of your app, please see the linked github repository.
  For a list of options you can pass to the flutter engine, look here:
    https://github.com/flutter/engine/blob/main/shell/common/switches.h
```

`<asset bundle path>` is the path of the flutter asset bundle directory (i.e. the directory containing `kernel_blob.bin`)
of the flutter app you're trying to run.

`[flutter engine options...]` will be passed as commandline arguments to the flutter engine. You can find a list of commandline options for the flutter engine [Here](https://github.com/flutter/engine/blob/master/shell/common/switches.h).

### gstreamer video player
Gstreamer video player is a newer video player based on gstreamer.

To use the gstreamer video player, just rebuild flutter-pi (delete your build folder and reconfigure) and make sure the necessary gstreamer packages are installed. (See [dependencies](#dependencies))

And then, just use the stuff in the official [video_player](https://pub.dev/packages/video_player) package. (`VideoPlayer`, `VideoPlayerController`, etc, there's nothing specific you need to do on the dart-side)

### audioplayers
As of current moment flutter-pi implements plugin for `audioplayers: ^5.0.0`.
There are several things you need to keep in mind:
- As flutter-pi is intended for use on constrained systems like raspberry pi, you should avoid creating multiple temporary instances and instead prefer to use one global instance of `AudioPlayer`. There is limit you can easily hit if you're going to spam multiple instances of `AudioPlayer`
- Plugin was tested to work with ALSA and `pulseaudio` might prevent the plugin from playing audio correctly:
    - Hence please make sure you delete `pulseaudio` package from your system.
    - Make sure you have `gstreamer1.0-alsa` package installed in addition to packages needed for gstreamer video player.
    - Make sure you can list audio devices using command: `aplay -L`
        - If there is error, please investigate why and fix it before using audio
        - One of the common reasons is outdated ALSA config in which case you should delete existing config and replace it with up to date one
- Finally, if you want to verify your audio setup is good, you can use `gst-launch` command to invoke `playbin` on audio file directly.

## 📊 Performance
### Graphics Performance
Graphics performance is actually pretty good. With most of the apps inside the `flutter SDK -> examples -> catalog` directory I get smooth 50-60fps on the Pi 4 2GB and Pi 3 A+.

### Touchscreen Latency
Due to the way the touchscreen driver works in raspbian, there's some delta between an actual touch of the touchscreen and a touch event arriving at userspace. The touchscreen driver in the raspbian kernel actually just repeatedly polls some buffer shared with the firmware running on the VideoCore, and the videocore repeatedly polls the touchscreen. (both at 60Hz) So on average, there's a delay of 17ms (minimum 0ms, maximum 34ms). Actually, the firmware is polling correctly at ~60Hz, but the linux driver is not because there's a bug. The linux side actually polls at 25Hz, which makes touch applications look terrible. (When you drag something in a touch application, but the application only gets new touch data at 25Hz, it'll look like the application itself is _redrawing_ at 25Hz, making it look very laggy) The github issue for this raspberry pi kernel bug is [here](https://github.com/raspberrypi/linux/issues/3777). Leave a like on the issue if you'd like to see this fixed in the kernel.

If you're not shy of some experimentation, you can try using this old (userspace) touchscreen driver: https://github.com/ardera/raspberrypi-fast-ts. I created it a while ago to improve latency and polling rate, but it's a bit out of date now. The difference is noticeable though, flutter apps look and feel a lot better when using it.

## 📦 Useful Dart Packages

| Package | Category    | Author | Description |
| - | - | - | - |
| flutterpi_tool ([package](https://pub.dev/packages/flutterpi_tool/)) ([repo](https://github.com/ardera/flutterpi_tool)) | 🔧 tooling | Hannes Winkler (me) | Tool to make developing & distributing apps for flutter-pi easier. |
| flutter_gpiod ([package](https://pub.dev/packages/flutter_gpiod/)) ([repo](https://github.com/ardera/flutter_packages/tree/main/packages/flutter_gpiod)) | 🖨 peripherals | Hannes Winkler | GPIO control support for dart/flutter, uses kernel interfaces directly for more performance. |
| linux_serial ([package](https://pub.dev/packages/linux_serial/)) ([repo](https://github.com/ardera/flutter_packages/tree/main/packages/linux_serial)) | 🖨 peripherals | Hannes Winkler | Serial Port support for dart/flutter, uses kernel interfaces directly for more performance. |
| linux_spidev ([package](https://pub.dev/packages/linux_spidev/)) ([repo](https://github.com/ardera/flutter_packages/tree/main/packages/linux_spidev)) | 🖨 peripherals | Hannes Winkler | SPI bus support for dart/flutter, uses kernel interfaces directly for more performance. |
| dart_periphery ([package](https://pub.dev/packages/dart_periphery)) ([repo](https://github.com/pezi/dart_periphery)) | 🖨 peripherals | [Peter Sauer](https://github.com/pezi/) | All-in-one package GPIO, I2C, SPI, Serial, PWM, Led, MMIO support using c-periphery. |
| flutterpi_gstreamer_video_player ([package](https://pub.dev/packages/flutterpi_gstreamer_video_player)) ([repo](https://github.com/ardera/flutter_packages/tree/main/packages/flutterpi_gstreamer_video_player)) | ⏯️ multimedia | Hannes Winkler | Official video player implementation for flutter-pi. See [GStreamer video player](#gstreamer-video-player) section above. |
| charset_converter ([package](https://pub.dev/packages/charset_converter)) ([repo](https://github.com/pr0gramista/charset_converter)) | 🗚 encoding | Bartosz Wiśniewski | Encode and decode charsets using platform built-in converter. |
| sentry_flutter ([package](https://pub.dev/packages/sentry_flutter)) ([repo](https://github.com/getsentry/sentry-dart))|  📊 Monitoring | sentry.io | See https://github.com/ardera/flutter-pi/wiki/Sentry-Support for instructions. |

## 💬 Discord
There a `#custom-embedders` channel on the [flutter discord](https://github.com/flutter/flutter/wiki/Chat) which you can use if you have any questions regarding flutter-pi or generally, anything related to embedding the engine for which you don't want to open issue about or write an email.
