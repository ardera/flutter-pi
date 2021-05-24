#
# MIT License
#
# Copyright (c) 2020 Joel Winarske
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#

cmake_minimum_required(VERSION 3.10.2)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: Debug, Release, or MinSizeRel." FORCE)
    message(STATUS "CMAKE_BUILD_TYPE not set, defaulting to Release.")
endif()

project(flutter-pi LANGUAGES C)

message(STATUS "Generator .............. ${CMAKE_GENERATOR}")
message(STATUS "Build Type ............. ${CMAKE_BUILD_TYPE}")

if(NOT FLUTTER_EMBEDDER_HEADER)

  include(FetchContent)

  if(NOT FLUTTER_ENGINE_SHA)

    if(NOT CHANNEL)
      set(CHANNEL "stable" CACHE STRING "The flutter channel to be used for downloading the flutter_embedder.h header file. Choose: master, dev, beta, stable" FORCE)
      message(STATUS "Flutter Channel not set, defaulting to stable")
    endif()

    message(STATUS "Flutter Channel ........ ${CHANNEL}")

    FetchContent_Declare(engine-version
      URL https://raw.githubusercontent.com/flutter/flutter/${CHANNEL}/bin/internal/engine.version
      DOWNLOAD_NAME engine.version
      DOWNLOAD_NO_EXTRACT TRUE
      DOWNLOAD_DIR ${CMAKE_BINARY_DIR}
    )

    FetchContent_GetProperties(engine-version)
    if(NOT engine-version_POPULATED)
      FetchContent_Populate(engine-version)
      file(READ ${CMAKE_BINARY_DIR}/engine.version FLUTTER_ENGINE_SHA)
      string(REPLACE "\n" "" FLUTTER_ENGINE_SHA ${FLUTTER_ENGINE_SHA})
    else()
      MESSAGE(FATAL "Unable to determine engine-version, please override FLUTTER_ENGINE_SHA")
    endif()

  endif()

  message(STATUS "Engine SHA1 ............ ${FLUTTER_ENGINE_SHA}")

  set(FLUTTER_EMBEDDER_HEADER ${CMAKE_BINARY_DIR}/flutter_embedder.h)

  # Download and setup the flutter engine library header.
  if(NOT EXISTS ${FLUTTER_EMBEDDER_HEADER})
    file(DOWNLOAD
      https://raw.githubusercontent.com/flutter/engine/${FLUTTER_ENGINE_SHA}/shell/platform/embedder/embedder.h
      ${FLUTTER_EMBEDDER_HEADER}
    )
  endif()
else()
  message(STATUS "Flutter Header ......... ${FLUTTER_EMBEDDER_HEADER}")
endif()

include(ExternalProject)

set(ENV{PKG_CONFIG_PATH} ${PKG_CONFIG_PATH})
message(STATUS "PKG_CONFIG_PATH ........ $ENV{PKG_CONFIG_PATH}")

include(FindPkgConfig)
pkg_check_modules(DRM REQUIRED libdrm)
pkg_check_modules(GBM REQUIRED gbm)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLESV2 REQUIRED glesv2)
pkg_check_modules(LIBSYSTEMD REQUIRED libsystemd)
pkg_check_modules(LIBINPUT REQUIRED libinput)
pkg_check_modules(LIBXKBCOMMON REQUIRED xkbcommon)
pkg_check_modules(LIBUDEV REQUIRED libudev)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
pkg_check_modules(LIBSSL REQUIRED libssl)
pkg_check_modules(LIBCRYPTO REQUIRED libcrypto)
pkg_check_modules(LIBGSTREAMER REQUIRED gstreamer-1.0)
pkg_check_modules(LIBGSTREAMER-APP REQUIRED gstreamer-app-1.0)
pkg_check_modules(LIBGSTREAMER-PLUGINS-BASE REQUIRED gstreamer-plugins-base-1.0)

set(BUILD_TEXT_INPUT_PLUGIN ON CACHE BOOL "Include the text input plugin in the finished binary. Enables text input (to flutter text fields, for example) via attached keyboards.")
set(BUILD_RAW_KEYBOARD_PLUGIN ON CACHE BOOL "Include the raw keyboard plugin in the finished binary. Enables raw keycode listening in flutter via the flutter RawKeyboard interface.")
set(BUILD_TEST_PLUGIN OFF CACHE BOOL "Include the test plugin in the finished binary. Allows testing platform channel communication.")
set(BUILD_OMXPLAYER_VIDEO_PLAYER_PLUGIN ON CACHE BOOL "Include the omxplayer_video_player plugin in the finished binary. Allows for hardware accelerated video playback in flutter using omxplayer.")
set(BUILD_ANDROID_AUTO_HEADUNIT_PLUGIN OFF CACHE BOOL "Include the android_auto_headunit plugin in the finished binary.")

set(FLUTTER_PI_SRC
  src/collection.c
  src/compositor.c
  src/cursor.c
  src/dylib_deps.c
  src/event_loop.c
  src/flutter-pi.c
  src/keyboard.c
  src/messenger.c
  src/modesetting.c
  src/platformchannel.c
  src/pluginregistry.c
  src/renderer.c
  src/texture_registry.c
  src/user_input.c
	src/plugins/services.c
)

if (BUILD_TEXT_INPUT_PLUGIN)
  list(APPEND FLUTTER_PI_SRC src/plugins/text_input.c)
endif()
if (BUILD_RAW_KEYBOARD_PLUGIN)
  list(APPEND FLUTTER_PI_SRC src/plugins/raw_keyboard.c)
endif()
if (BUILD_TEST_PLUGIN)
  list(APPEND FLUTTER_PI_SRC src/plugins/testplugin.c)
endif()
if (BUILD_OMXPLAYER_VIDEO_PLAYER_PLUGIN)
  list(APPEND FLUTTER_PI_SRC src/plugins/omxplayer_video_player.c)
endif()
if (BUILD_ANDROID_AUTO_HEADUNIT_PLUGIN)
  list(APPEND FLUTTER_PI_SRC src/plugins/android_auto/android_auto.c)
  list(APPEND FLUTTER_PI_SRC src/plugins/android_auto/aa_channel.c)
  list(APPEND FLUTTER_PI_SRC src/plugins/android_auto/aa_xfer.c)
  list(APPEND FLUTTER_PI_SRC src/plugins/android_auto/aa_device.c)
endif()

add_executable(flutter-pi ${FLUTTER_PI_SRC})

target_link_libraries(flutter-pi
  ${DRM_LDFLAGS}
  ${GBM_LDFLAGS}
  ${EGL_LDFLAGS}
  ${GLESV2_LDFLAGS}
  ${LIBSYSTEMD_LDFLAGS}
  ${LIBINPUT_LDFLAGS}
  ${LIBUDEV_LDFLAGS}
  ${LIBXKBCOMMON_LDFLAGS}
  ${LIBUSB_LDFLAGS}
  ${LIBSSL_LDFLAGS}
  ${LIBCRYPTO_LDFLAGS}
  ${LIBGSTREAMER_LDFLAGS}
  pthread dl rt m
)

target_include_directories(flutter-pi PRIVATE
  ${CMAKE_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/include/plugins
  ${DRM_INCLUDE_DIRS}
  ${GBM_INCLUDE_DIRS}
  ${EGL_INCLUDE_DIRS}
  ${GLESV2_INCLUDE_DIRS}
  ${LIBSYSTEMD_INCLUDE_DIRS}
  ${LIBINPUT_INCLUDE_DIRS}
  ${LIBUDEV_INCLUDE_DIRS}
  ${LIBXKBCOMMON_INCLUDE_DIRS}
  ${LIBUSB_INCLUDE_DIRS}
  ${LIBSSL_INCLUDE_DIRS}
  ${LIBCRYPTO_INCLUDE_DIRS}
  ${LIBGSTREAMER_INCLUDE_DIRS}
)

target_compile_options(flutter-pi PRIVATE
  ${DRM_CFLAGS}  
  ${GBM_CFLAGS}
  ${EGL_CFLAGS}
  ${GLESV2_CFLAGS}
  ${LIBSYSTEMD_CFLAGS}
  ${LIBINPUT_CFLAGS}
  ${LIBUDEV_CFLAGS}
  ${LIBXKBCOMMON_CFLAGS}
  ${LIBUSB_CFLAGS}
  ${LIBSSL_CFLAGS}
  ${LIBCRYPTO_CFLAGS}
  ${LIBGSTREAMER_CFLAGS}
  $<$<CONFIG:DEBUG>:-O0 -ggdb -Werror -Wall -Wextra -Wno-error=missing-field-initializers -fsanitize=undefined -fno-omit-frame-pointer>
)

if (BUILD_TEXT_INPUT_PLUGIN)
  target_compile_definitions(flutter-pi PRIVATE "BUILD_TEXT_INPUT_PLUGIN")
endif()
if (BUILD_RAW_KEYBOARD_PLUGIN)
  target_compile_definitions(flutter-pi PRIVATE "BUILD_RAW_KEYBOARD_PLUGIN")
endif()
if (BUILD_TEST_PLUGIN)
  target_compile_definitions(flutter-pi PRIVATE "BUILD_TEST_PLUGIN")
endif()
if (BUILD_OMXPLAYER_VIDEO_PLAYER_PLUGIN)
  target_compile_definitions(flutter-pi PRIVATE "BUILD_OMXPLAYER_VIDEO_PLAYER_PLUGIN")
endif()
if (BUILD_ANDROID_AUTO_HEADUNIT_PLUGIN)
  target_compile_definitions(flutter-pi PRIVATE "BUILD_ANDROID_AUTO_HEADUNIT_PLUGIN")
endif()

target_compile_definitions(flutter-pi PRIVATE "HAS_FBDEV")
target_compile_definitions(flutter-pi PRIVATE "HAS_KMS")
target_compile_definitions(flutter-pi PRIVATE "HAS_GBM")
target_compile_definitions(flutter-pi PRIVATE "HAS_EGL")

target_link_options(flutter-pi PRIVATE
  -rdynamic
# $<$<CONFIG:DEBUG>:-fsanitize=address -static-libasan>
  $<$<CONFIG:DEBUG>:-fsanitize=undefined>
)

if ("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    target_compile_options(flutter-pi PRIVATE -fdiagnostics-color=always)
elseif ("${CMAKE_C_COMPILER_ID}" STREQUAL "Clang")
    target_compile_options(flutter-pi PRIVATE -fcolor-diagnostics)
endif()

install(TARGETS flutter-pi RUNTIME DESTINATION bin)
