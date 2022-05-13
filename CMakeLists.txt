#
# MIT License
#
# Original work Copyright (c) 2020 Joel Winarske
# Modified work Copyright     2021 Hannes Winkler
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

project(flutter-pi LANGUAGES C CXX VERSION "1.0.0")

message(STATUS "Generator .............. ${CMAKE_GENERATOR}")
message(STATUS "Build Type ............. ${CMAKE_BUILD_TYPE}")

option(OMXPLAYER_SUPPORTS_RUNTIME_ROTATION "Whether omxplayer supports runtime rotation." OFF)
option(BUILD_TEXT_INPUT_PLUGIN "Include the text input plugin in the finished binary. Enables text input (to flutter text fields, for example) via attached keyboards." ON)
option(BUILD_RAW_KEYBOARD_PLUGIN "Include the raw keyboard plugin in the finished binary. Enables raw keycode listening in flutter via the flutter RawKeyboard interface." ON)
option(BUILD_TEST_PLUGIN "Include the test plugin in the finished binary. Allows testing platform channel communication." OFF)
option(BUILD_OMXPLAYER_VIDEO_PLAYER_PLUGIN "Include the omxplayer_video_player plugin in the finished binary. Allows for hardware accelerated video playback in flutter using omxplayer." ON)
option(BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN "Include the gstreamer_video_player plugin in the finished binary. Allows for more stable, hardware accelerated video playback in flutter using gstreamer." ON)
option(TRY_BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN "Don't throw an error if the gstreamer libs aren't found, instead just don't build the gstreamer video player plugin in that case." ON)
option(DUMP_ENGINE_LAYERS "True if flutter-pi should dump the list of rendering layers that the flutter engine sends to flutter-pi on each draw." OFF)
option(ENABLE_TSAN "True to build & link with -fsanitize=thread" OFF)
option(ENABLE_ASAN "True to build & link with -fsanitize=address" OFF)
option(ENABLE_UBSAN "True to build & link with -fsanitize=undefined" OFF)
option(ENABLE_MTRACE "True if flutter-pi should call GNU mtrace() on startup." OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
include(CheckCCompilerFlag)

include(FindPkgConfig)
pkg_check_modules(DRM REQUIRED libdrm)
pkg_check_modules(GBM REQUIRED gbm)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLESV2 REQUIRED glesv2)
pkg_check_modules(LIBSYSTEMD REQUIRED libsystemd)
pkg_check_modules(LIBINPUT REQUIRED libinput)
pkg_check_modules(LIBXKBCOMMON REQUIRED xkbcommon)
pkg_check_modules(LIBUDEV REQUIRED libudev)

add_executable(flutter-pi
  src/flutter-pi.c
  src/platformchannel.c
  src/pluginregistry.c
  src/texture_registry.c
  src/compositor.c
  src/modesetting.c
  src/collection.c
  src/cursor.c
  src/keyboard.c
  src/user_input.c
  src/locales.c
  src/notifier_listener.c
  src/pixel_format.c
  src/plugins/services.c
)

target_link_libraries(flutter-pi
  ${DRM_LDFLAGS}
  ${GBM_LDFLAGS}
  ${EGL_LDFLAGS}
  ${GLESV2_LDFLAGS}
  EGL 
  systemd #${LIBSYSTEMD_LDFLAGS}
  input #${LIBINPUT_LDFLAGS}
  xkbcommon #${LIBUDEV_LDFLAGS}
  udev #${LIBXKBCOMMON_LDFLAGS}
  pthread dl rt m atomic
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
  $<$<CONFIG:Debug>:-O0 -Wall -Wextra -Wno-unused-function -Wno-sign-compare -Wno-missing-field-initializers -Werror -ggdb -U_FORTIFY_SOURCE -DDEBUG>
  $<$<CONFIG:RelWithDebInfo>:-O2 -Wall -Wextra -Wno-unused-function -Wno-sign-compare -Wno-missing-field-initializers -ggdb>
  $<$<CONFIG:Release>:-O2 -Wall -Wextra -Wno-unused-function -Wno-sign-compare -Wno-missing-field-initializers -ggdb>
)

# TODO: Just unconditionally define those, make them optional later 
target_compile_definitions(flutter-pi PRIVATE HAS_KMS HAS_EGL HAS_GBM HAS_FBDEV)

# TODO: We actually don't need the compile definitions anymore, except for
# text input and raw keyboard plugin (because those have special treatment
# in flutter-pi.c)
if (BUILD_TEXT_INPUT_PLUGIN)
  target_sources(flutter-pi PRIVATE src/plugins/text_input.c)
  target_compile_definitions(flutter-pi PRIVATE "BUILD_TEXT_INPUT_PLUGIN")
endif()
if (BUILD_RAW_KEYBOARD_PLUGIN)
  target_sources(flutter-pi PRIVATE src/plugins/raw_keyboard.c)
  target_compile_definitions(flutter-pi PRIVATE "BUILD_RAW_KEYBOARD_PLUGIN")
endif()
if (BUILD_TEST_PLUGIN)
  target_sources(flutter-pi PRIVATE src/plugins/testplugin.c)
  target_compile_definitions(flutter-pi PRIVATE "BUILD_TEST_PLUGIN")
endif()
if (BUILD_OMXPLAYER_VIDEO_PLAYER_PLUGIN)
  target_sources(flutter-pi PRIVATE src/plugins/omxplayer_video_player.c)
  target_compile_definitions(flutter-pi PRIVATE "BUILD_OMXPLAYER_VIDEO_PLAYER_PLUGIN")
endif()
if (OMXPLAYER_SUPPORTS_RUNTIME_ROTATION)
  target_compile_definitions(flutter-pi PRIVATE "OMXPLAYER_SUPPORTS_RUNTIME_ROTATION")
endif()
if (BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN)
  if (TRY_BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN)
    pkg_check_modules(LIBGSTREAMER gstreamer-1.0)
    pkg_check_modules(LIBGSTREAMER_PLUGINS_BASE gstreamer-plugins-base-1.0)
    pkg_check_modules(LIBGSTREAMER_APP gstreamer-app-1.0)
    pkg_check_modules(LIBGSTREAMER_ALLOCATORS gstreamer-allocators-1.0)
    pkg_check_modules(LIBGSTREAMER_VIDEO gstreamer-video-1.0)
  else()
    pkg_check_modules(LIBGSTREAMER REQUIRED gstreamer-1.0)
    pkg_check_modules(LIBGSTREAMER_PLUGINS_BASE REQUIRED gstreamer-plugins-base-1.0)
    pkg_check_modules(LIBGSTREAMER_APP REQUIRED gstreamer-app-1.0)
    pkg_check_modules(LIBGSTREAMER_ALLOCATORS REQUIRED gstreamer-allocators-1.0)
    pkg_check_modules(LIBGSTREAMER_VIDEO REQUIRED gstreamer-video-1.0)
  endif()

  if (LIBGSTREAMER_FOUND AND LIBGSTREAMER_PLUGINS_BASE_FOUND AND LIBGSTREAMER_APP_FOUND AND LIBGSTREAMER_ALLOCATORS_FOUND AND LIBGSTREAMER_VIDEO_FOUND)
    target_sources(flutter-pi PRIVATE
      src/plugins/gstreamer_video_player/plugin.c
      src/plugins/gstreamer_video_player/player.c
      src/plugins/gstreamer_video_player/frame.c
    )
    target_compile_definitions(flutter-pi PRIVATE "BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN")
    target_link_libraries(flutter-pi
      ${LIBGSTREAMER_LDFLAGS}
      ${LIBGSTREAMER_PLUGINS_BASE_LDFLAGS}
      ${LIBGSTREAMER_APP_LDFLAGS}
      ${LIBGSTREAMER_ALLOCATORS_LDFLAGS}
      ${LIBGSTREAMER_VIDEO_LDFLAGS}
    )
    target_include_directories(flutter-pi PRIVATE
      ${LIBGSTREAMER_INCLUDE_DIRS}
      ${LIBGSTREAMER_PLUGINS_BASE_INCLUDE_DIRS}
      ${LIBGSTREAMER_APP_INCLUDE_DIRS}
      ${LIBGSTREAMER_ALLOCATORS_INCLUDE_DIRS}
      ${LIBGSTREAMER_VIDEO_INCLUDE_DIRS}
    )
    target_compile_options(flutter-pi PRIVATE
      ${LIBGSTREAMER_CFLAGS}
      ${LIBGSTREAMER_PLUGINS_BASE_CFLAGS}
      ${LIBGSTREAMER_APP_CFLAGS}
      ${LIBGSTREAMER_ALLOCATORS_CFLAGS}
      ${LIBGSTREAMER_VIDEO_CFLAGS}
    )
  else()
    message(NOTICE "Couldn't find gstreamer libraries. Gstreamer video player plugin won't be build.")
  endif()
endif()

# Needed so dart VM can actually resolve symbols in the same
# executable.
target_link_options(flutter-pi PRIVATE
  -rdynamic
)

if (ENABLE_TSAN)
  target_link_options(flutter-pi PRIVATE -fsanitize=thread)
  target_compile_options(flutter-pi PRIVATE -fsanitize=thread)
endif()
if (ENABLE_ASAN)
  # when we use asan, we need to force linking against the C++ stdlib.
  # If we don't link against it, and load a dynamic library that's linked against the C++ stdlib (like the flutter engine),
  # and something in the dynamically loaded library triggers asan, it'll throw an error because it hasn't
  # intercepted stdc++ yet.
  # Also disable --as-needed so we _actually_ link against c++, even though we don't use any symbols from it.
  target_link_libraries(flutter-pi stdc++)
  target_link_options(flutter-pi PRIVATE -fsanitize=address -fno-omit-frame-pointer -Wl,--no-as-needed)
  target_compile_options(flutter-pi PRIVATE -fsanitize=address)

  check_c_compiler_flag(-static-libasan HAS_STATIC_LIBASAN)
  if (HAS_STATIC_LIBASAN)
    target_link_options(flutter-pi PRIVATE -static-libasan)
  endif()
endif()
if (ENABLE_UBSAN)
  target_link_options(flutter-pi PRIVATE -fsanitize=undefined)
  target_compile_options(flutter-pi PRIVATE -fsanitize=undefined)
endif()
if (ENABLE_MTRACE)
  target_compile_definitions(flutter-pi PRIVATE "ENABLE_MTRACE")
endif()

install(TARGETS flutter-pi RUNTIME DESTINATION bin)
