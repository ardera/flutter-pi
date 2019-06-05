# flutter-pi
A light-weight, single-file Flutter Engine Embedder for Raspberry Pi that's using the broadcom APIs. Inspired by https://github.com/chinmaygarde/flutter_from_scratch.

## Running
Run using
```bash
./flutter-pi /path/without/trailing/slash [flutter arguments...]
```
where `/path/without/trailing/slash` is the path of the flutter asset bundle directory (i.e. the directory containing the kernel_blob.bin)
of the flutter app you're trying to run.

The `[flutter arguments...]` will be passed as commandline arguments to the flutter engine.

## Compiling
You first need a valid `libflutter_engine.so`. [Here](https://medium.com/flutter/flutter-on-raspberry-pi-mostly-from-scratch-2824c5e7dcb1)
are some rough guidelines on how to build it.
```bash
cc -D_GNU_SOURCE \
  -lrt -lbrcmGLESv2 -lflutter_engine -lpthread -ldl -lbcm_host -lvcos -lvchiq_arm -lm \
  ./main.c -o ./flutter-pi
```
## Cross-Compiling
You need a valid `libflutter_engine.so`, `flutter_embedder.h`, a valid raspberry pi sysroot including the /opt directory, and a valid toolchain targeting
arm-linux-gnueabihf. Then execute:
```bash
/path/to/cross_c_compiler \
  -D_GNU_SOURCE \
  --sysroot /path/to/sysroot \
  -I/path/to/sysroot/opt/vc/include \
  -I/directory/containing/flutter_embedder.h/ \
  -L/path/to/sysroot/opt/vc/lib \
  -L/directory/containing/libflutter_engine.so/ \
  -lrt -lbrcmEGL -lbrcmGLESv2 -lflutter_engine -lpthread -ldl -lbcm_host -lvcos -lvchiq_arm -lm \
  ./main.c -o ./flutter-rpi
```
