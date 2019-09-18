# flutter-pi
A light-weight Flutter Engine Embedder for Raspberry Pi. Inspired by https://github.com/chinmaygarde/flutter_from_scratch.
Flutter-pi also runs without X11, so you don't need to boot into Raspbian Desktop & have X11 and LXDE load up; just boot into the command-line.

Currently supported are basic, pure-dart Apps (not using any plugins), mouse input (no mouse cursor yet), touchscreen input, and the StandardMethodCodec method-channels (currently needs fixing).
Not yet supported are JSON method-channels. Generally, flutter-pi is not yet ready to be used as a base for your project.

## Running
This branch (feature-v3d-anholt) doesn't support the legacy GL driver anymore. You need to activate the anholt v3d driver in raspi-config. Go to raspi-config -> Advanced -> GL Driver -> and select fake-KMS. Full-KMS is a bit buggy and doesn't work with the Raspberry Pi 7" display (or generally, any DSI display).

For some reason performance is much better when I gave the GPU only 16M RAM in fake-kms. I don't know why.

Also, you need to tell flutter-pi which input device to use and whether it's a touchscreen or mouse. Input devices are typically located at `/dev/input/...`. Just run `evtest` (`sudo apt install evtest`) to find out which exact path you should use. Currently only one input device is supported by flutter-pi.

Run using
```bash
./flutter-pi [flutter-pi options...] /path/without/trailing/slash [flutter engine arguments...]
```

`[flutter-pi options...]` are:
- `-t /path/to/device` where `/path/to/device` is a path to a touchscreen input device (typically `/dev/input/event0` or similiar)
- `-m /path/to/device` where `/path/to/device` is a path to a mouse input device (typically `/dev/input/mouse0` or `/dev/input/event0` or similiar)

`/path/without/trailing/slash` is the path of the flutter asset bundle directory (i.e. the directory containing the kernel_blob.bin)
of the flutter app you're trying to run.

`[flutter engine arguments...]` will be passed as commandline arguments to the flutter engine. You can find a list of commandline options for the flutter engine [Here](https://github.com/flutter/engine/blob/master/shell/common/switches.h);

## Building the asset bundle
You need a correctly installed flutter SDK. (i.e. the `flutter` tool must be in your PATH)

Example for flutter_gallery: (note that the flutter_gallery example doesn't work with flutter-pi, since it requires plugins)
```bash
cd flutter/examples/flutter_gallery
flutter build bundle
```
After that `flutter/examples/flutter_gallery/build/flutter_assets` would be a valid path to pass as an argument to flutter-pi.

## Compiling (on the Raspberry Pi)
You first need a `libflutter_engine.so` and `flutter_embedder.h`. [Here](https://medium.com/flutter/flutter-on-raspberry-pi-mostly-from-scratch-2824c5e7dcb1)
are some rough guidelines on how to build it. (Note: the icudtl.dat that is generated during the engine compilation needs to be on the RPi too, but it's not needed for compilation of flutter-pi)

You also need some dependencies; run `sudo apt install libgl1-mesa-dev libgles2-mesa-dev libegl-meso0 libdrm-dev libgbm-dev`.

Compiling the embedder:
```bash
mkdir out
cc -D_GNU_SOURCE \
  `pkg-config --cflags --libs dri gbm libdrm glesv2 egl` -lrt -lflutter_engine -lpthread -ldl \
  ./src/flutter-pi.c ./src/methodchannel.c -o ./out/flutter-pi
```

## Performance
Performance is actually better than I expected. With most of the apps inside the `flutter SDK -> examples -> catalog` directory I get smooth 50-60fps.

## Touchscreen Bug
~~If you use the official 7 inch touchscreen, performance will feel much worse while dragging something. This seems to be some bug in the touchscreen driver. The embedder / userspace only gets around 25 touch events a second, meaning that while dragging something (like in tabbed_app_bar.dart), the position of the object being dragged is only updated 25 times a second. This results in the app looking like it runs at 25fps. The touchscreen could do up to 100 touch updates a second though.~~

[This has been fixed.](https://github.com/raspberrypi/linux/issues/3227) If you want to get the fix, you can run (rpi-update)[https://github.com/hexxeh/rpi-update], which will update your system to the newest version.

