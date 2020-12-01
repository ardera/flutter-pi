# Getting Started with flutter-pi

These instructions summarize the information in the [README.md](README.md) file. See that file for more details.
In each step below with Bash commands, the commands start with a set of `export` commands that you should update appropriately.

1. Build a Raspberry Pi with a touchscreen. This will be your target.

2. Prepare your target for flutter-pi (see "Configuring your Raspberry Pi" in the README for details):
   ```bash
   export APPNAME=hello_pi # change this to the name of your application
   
   # one-time setup
   sudo usermod -a -G render $USER
   sudo apt --yes install libgl1-mesa-dev libgles2-mesa-dev libegl-mesa0 libdrm-dev libgbm-dev
   sudo apt --yes install gpiod libgpiod-dev libsystemd-dev libinput-dev libudev-dev libxkbcommon-dev
   sudo apt --yes install ttf-mscorefonts-installer fontconfig
   sudo fc-cache
   if [ `uname -m` == 'armv7l' ]; then export ARM=arm; else export ARM=arm64; fi
   mkdir -p ~/dev
   pushd ~/dev
   git clone --depth 1 --branch engine-binaries https://github.com/ardera/flutter-pi.git engine-binaries
   sudo cp ~/dev/engine-binaries/$ARM/libflutter_engine.so.* /usr/lib
   sudo cp ~/dev/engine-binaries/$ARM/icudtl.dat /usr/lib
   sudo cp ~/dev/engine-binaries/flutter_embedder.h /usr/include
   git clone https://github.com/ardera/flutter-pi.git
   cd flutter-pi; make
   # per-application setup
   mkdir -p ~/dev/$APPNAME
   popd
   echo You will need to set ARM to: $ARM
   ```
   
   Take a note of the last line of output. It should say you need "arm" or "arm64". This is used to set ARM below.
   
   Take a note of which version of Flutter the binaries were compiled for. This is used to set VERSION below. It should be clear from the commit messages of the latest commit to the repo: https://github.com/ardera/flutter-pi/tree/engine-binaries

3. Configure your target. Run `sudo raspi-config`, and configure the system as follows:
   1. Select `Boot Options` -> `Desktop / CLI` -> `Console` (or `Console (Autologin)`).
   2. Select `Advanced Options` -> `GL Driver` -> `GL (Fake-KMS)`.
   3. Select `Advanced Options` -> `Memory Split` and set it to `16`.
   4. Exit `raspi-config` and reboot when offered.

4. Download, install, and configure Flutter on a host machine (not the Raspberry Pi), then create an application, compile it, and run it.
   These instructions will put the version of Flutter you will use for the Raspberry Pi into the `~/dev/flutter-for-pi` directory so as to not interfere with your normal Flutter installation.
   For the purposes of these instructions we'll assume this is an x64 Linux workstation.
   ```bash
   export VERSION=... # set this to the version determined above, e.g. 1.22.4
   export ARM=... # set this to "arm" or "arm64" as determined above
   export TARGET=... # set this to your Raspberry Pi's hostname
   export APPNAME=hello_pi # same as what you used earlier
   export TARGETUSER=pi # set this to your username on the raspberry pi, e.g. "pi" or $USER if it's the same as on the host
   
   mkdir -p ~/dev
   pushd ~/dev
   # one-time setup
   git clone --branch $VERSION https://github.com/flutter/flutter.git flutter-for-pi
   ~/dev/flutter-for-pi/bin/flutter precache
   git clone --depth 1 --branch engine-binaries https://github.com/ardera/flutter-pi.git engine-binaries
   chmod +x engine-binaries/$ARM/gen_snapshot_linux_x64
   # create the application
   flutter-for-pi/bin/flutter create $APPNAME
   # compile the application
   cd $APPNAME
   ../flutter-for-pi/bin/flutter packages get # this might not be necessary
   ../flutter-for-pi/bin/flutter build bundle --no-tree-shake-icons --precompiled
   ../flutter-for-pi/bin/cache/dart-sdk/bin/dart \
     ../flutter-for-pi/bin/cache/dart-sdk/bin/snapshots/frontend_server.dart.snapshot \
     --sdk-root ~/dev/flutter-for-pi/bin/cache/artifacts/engine/common/flutter_patched_sdk_product \
     --target=flutter \
     --aot --tfa -Ddart.vm.product=true \
     --packages .packages --output-dill build/kernel_snapshot.dill --depfile build/kernel_snapshot.d \
     package:$APPNAME/main.dart
   ../engine-binaries/$ARM/gen_snapshot_linux_x64 \
     --causal_async_stacks --deterministic --snapshot_kind=app-aot-elf \
     --strip --sim_use_hardfp --no-use-integer-division \
     --elf=build/app.so build/kernel_snapshot.dill
   # upload the application
   rsync --recursive ~/dev/$APPNAME/build/flutter_assets/ $TARGETUSER@$TARGET:dev/$APPNAME
   scp ~/dev/$APPNAME/build/app.so $TARGETUSER@$TARGET:dev/$APPNAME/app.so
   # run the application
   ssh $TARGETUSER@$TARGET "killall" "flutter-pi"	
   ssh $TARGETUSER@$TARGET "dev/flutter-pi/out/flutter-pi" "--release" "~/dev/$APPNAME"
   popd
   ```

That's it!
