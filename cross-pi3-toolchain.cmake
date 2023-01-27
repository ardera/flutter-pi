set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_SYSROOT /home/hannes/devel/hpi3-sysroot)
set(CMAKE_STAGING_PREFIX /home/hannes/devel/hpi3-sysroot/home/pi/devel/flutterpi-install)

set(CMAKE_C_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET arm-linux-gnueabihf)

set(CMAKE_FIND_ROOT_PATH /home/hannes/devel/hpi3-sysroot)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

add_compile_options("-march=armv7" "-mcpu=cortex-a53" "-mtune=cortex-a53")
add_link_options("-fuse-ld=lld")

set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig:${CMAKE_SYSROOT}/usr/lib/arm-linux-gnueabihf/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} ${CMAKE_SYSROOT})
