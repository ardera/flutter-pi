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

project(flutter-pi LANGUAGES C VERSION "1.0.0")

message(STATUS "Generator .............. ${CMAKE_GENERATOR}")
message(STATUS "Build Type ............. ${CMAKE_BUILD_TYPE}")

option(BUILD_TEXT_INPUT_PLUGIN "Include the text input plugin in the finished binary. Enables text input (to flutter text fields, for example) via attached keyboards." ON)
option(BUILD_RAW_KEYBOARD_PLUGIN "Include the raw keyboard plugin in the finished binary. Enables raw keycode listening in flutter via the flutter RawKeyboard interface." ON)
option(BUILD_TEST_PLUGIN "Include the test plugin in the finished binary. Allows testing platform channel communication." OFF)
option(BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN "Include the gstreamer based video plugins in the finished binary. Allows for more stable, hardware accelerated video playback in flutter using gstreamer." ON)
option(BUILD_GSTREAMER_AUDIO_PLAYER_PLUGIN "Include the gstreamer based audio plugins in the finished binary." ON)
option(TRY_BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN "Don't throw an error if the gstreamer libs aren't found, instead just don't build the gstreamer video player plugin in that case." ON)
option(TRY_BUILD_GSTREAMER_AUDIO_PLAYER_PLUGIN "Don't throw an error if the gstreamer libs aren't found, instead just don't build gstreamer audio plugin." ON)
option(DUMP_ENGINE_LAYERS "True if flutter-pi should dump the list of rendering layers that the flutter engine sends to flutter-pi on each draw." OFF)
option(ENABLE_TSAN "True to build & link with -fsanitize=thread" OFF)
option(ENABLE_ASAN "True to build & link with -fsanitize=address" OFF)
option(ENABLE_UBSAN "True to build & link with -fsanitize=undefined" OFF)
option(ENABLE_MTRACE "True if flutter-pi should call GNU mtrace() on startup." OFF)
option(ENABLE_TESTS "True if tests should be built. Requires Unity to be checked out at third_party/Unity." OFF)

set(FILESYSTEM_LAYOUTS default meta-flutter)
set(FILESYSTEM_LAYOUT "default" CACHE STRING "Where to look for the icudtl.dat, app.so/libapp.so, flutter asset bundle.")
set_property(CACHE FILESYSTEM_LAYOUT PROPERTY STRINGS ${FILESYSTEM_LAYOUTS})

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
pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GLESV2 REQUIRED IMPORTED_TARGET glesv2)
pkg_check_modules(LIBSYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
pkg_check_modules(LIBINPUT REQUIRED IMPORTED_TARGET libinput)
pkg_check_modules(LIBXKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(LIBUDEV REQUIRED IMPORTED_TARGET libudev)

# find pthreads
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_library(
  flutterpi_module OBJECT
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
  src/filesystem_layout.c
  src/plugins/services.c
)

target_link_libraries(flutterpi_module PUBLIC
  PkgConfig::DRM
  PkgConfig::GBM
  PkgConfig::EGL
  PkgConfig::GLESV2
  PkgConfig::LIBSYSTEMD
  PkgConfig::LIBINPUT
  PkgConfig::LIBXKBCOMMON
  PkgConfig::LIBUDEV
  Threads::Threads
  ${CMAKE_DL_LIBS}
  rt m atomic
)

target_include_directories(flutterpi_module PUBLIC
  ${CMAKE_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/include/plugins
)

target_compile_options(flutterpi_module PRIVATE
  $<$<CONFIG:Debug>:-O0 -Wall -Wextra -Wno-sign-compare -Werror -ggdb -U_FORTIFY_SOURCE -DDEBUG>
  $<$<CONFIG:RelWithDebInfo>:-O2 -Wall -Wextra -Wno-sign-compare -ggdb>
  $<$<CONFIG:Release>:-O2 -Wall -Wextra -Wno-sign-compare -ggdb>
)

# GCC prior to 11.3 reports false positives for missing-field-initializers warning.
if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
  if (CMAKE_C_COMPILER_VERSION VERSION_LESS "11.3")
    target_compile_options(flutterpi_module PRIVATE -Wno-missing-field-initializers)
  endif()
endif()

# There's no other way to query the libinput version (in code) somehow.
# So we need to roll our own libinput version macro
string(REPLACE "." ";" LIBINPUT_VERSION_AS_LIST ${LIBINPUT_VERSION})
list(GET LIBINPUT_VERSION_AS_LIST 0 LIBINPUT_VERSION_MAJOR)
list(GET LIBINPUT_VERSION_AS_LIST 1 LIBINPUT_VERSION_MINOR)
list(GET LIBINPUT_VERSION_AS_LIST 2 LIBINPUT_VERSION_PATCH)

target_compile_definitions(flutterpi_module PRIVATE
  LIBINPUT_VERSION_MAJOR=${LIBINPUT_VERSION_MAJOR}
  LIBINPUT_VERSION_MINOR=${LIBINPUT_VERSION_MINOR}
  LIBINPUT_VERSION_PATCH=${LIBINPUT_VERSION_PATCH}
)

# TODO: Just unconditionally define those, make them optional later
target_compile_definitions(flutterpi_module PRIVATE HAS_KMS HAS_EGL HAS_GBM HAS_FBDEV)

if(NOT FILESYSTEM_LAYOUT IN_LIST FILESYSTEM_LAYOUTS)
  message(FATAL_ERROR "FILESYSTEM_LAYOUT must be one of ${FILESYSTEM_LAYOUTS}")
endif()

message(STATUS "Filesystem Layout ...... ${FILESYSTEM_LAYOUT}")

if(FILESYSTEM_LAYOUT STREQUAL default)
  target_compile_definitions(flutterpi_module PRIVATE "FILESYSTEM_LAYOUT_DEFAULT")
elseif(FILESYSTEM_LAYOUT STREQUAL meta-flutter)
  target_compile_definitions(flutterpi_module PRIVATE "FILESYSTEM_LAYOUT_METAFLUTTER")
endif()

# TODO: We actually don't need the compile definitions anymore, except for
# text input and raw keyboard plugin (because those have special treatment
# in flutter-pi.c)
if (BUILD_TEXT_INPUT_PLUGIN)
  target_sources(flutterpi_module PRIVATE src/plugins/text_input.c)
  target_compile_definitions(flutterpi_module PRIVATE "BUILD_TEXT_INPUT_PLUGIN")
endif()
if (BUILD_RAW_KEYBOARD_PLUGIN)
  target_sources(flutterpi_module PRIVATE src/plugins/raw_keyboard.c)
  target_compile_definitions(flutterpi_module PRIVATE "BUILD_RAW_KEYBOARD_PLUGIN")
endif()
if (BUILD_TEST_PLUGIN)
  target_sources(flutterpi_module PRIVATE src/plugins/testplugin.c)
endif()
if (BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN)
  if (TRY_BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN)
    pkg_check_modules(LIBGSTREAMER IMPORTED_TARGET gstreamer-1.0)
    pkg_check_modules(LIBGSTREAMER_PLUGINS_BASE IMPORTED_TARGET gstreamer-plugins-base-1.0)
    pkg_check_modules(LIBGSTREAMER_APP IMPORTED_TARGET gstreamer-app-1.0)
    pkg_check_modules(LIBGSTREAMER_ALLOCATORS IMPORTED_TARGET gstreamer-allocators-1.0)
    pkg_check_modules(LIBGSTREAMER_VIDEO IMPORTED_TARGET gstreamer-video-1.0)
  else()
    pkg_check_modules(LIBGSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0)
    pkg_check_modules(LIBGSTREAMER_PLUGINS_BASE REQUIRED IMPORTED_TARGET gstreamer-plugins-base-1.0)
    pkg_check_modules(LIBGSTREAMER_APP REQUIRED IMPORTED_TARGET gstreamer-app-1.0)
    pkg_check_modules(LIBGSTREAMER_ALLOCATORS REQUIRED IMPORTED_TARGET gstreamer-allocators-1.0)
    pkg_check_modules(LIBGSTREAMER_VIDEO REQUIRED IMPORTED_TARGET gstreamer-video-1.0)
  endif()

  if (LIBGSTREAMER_FOUND AND LIBGSTREAMER_PLUGINS_BASE_FOUND AND LIBGSTREAMER_APP_FOUND AND LIBGSTREAMER_ALLOCATORS_FOUND AND LIBGSTREAMER_VIDEO_FOUND)
    # There's no other way to query the libinput version (in code) somehow.
    # So we need to roll our own libinput version macro
    string(REPLACE "." ";" LIBGSTREAMER_VERSION_AS_LIST ${LIBGSTREAMER_VERSION})
    list(GET LIBGSTREAMER_VERSION_AS_LIST 0 LIBGSTREAMER_VERSION_MAJOR)
    list(GET LIBGSTREAMER_VERSION_AS_LIST 1 LIBGSTREAMER_VERSION_MINOR)
    list(GET LIBGSTREAMER_VERSION_AS_LIST 2 LIBGSTREAMER_VERSION_PATCH)

    target_compile_definitions(flutterpi_module PRIVATE
      LIBGSTREAMER_VERSION_MAJOR=${LIBGSTREAMER_VERSION_MAJOR}
      LIBGSTREAMER_VERSION_MINOR=${LIBGSTREAMER_VERSION_MINOR}
      LIBGSTREAMER_VERSION_PATCH=${LIBGSTREAMER_VERSION_PATCH}
    )
    target_sources(flutterpi_module PRIVATE
      src/plugins/gstreamer_video_player/plugin.c
      src/plugins/gstreamer_video_player/player.c
      src/plugins/gstreamer_video_player/frame.c
    )
    target_link_libraries(flutterpi_module PUBLIC
      PkgConfig::LIBGSTREAMER
      PkgConfig::LIBGSTREAMER_PLUGINS_BASE
      PkgConfig::LIBGSTREAMER_APP
      PkgConfig::LIBGSTREAMER_ALLOCATORS
      PkgConfig::LIBGSTREAMER_VIDEO
    )
  else()
    message(NOTICE "Couldn't find gstreamer libraries. Gstreamer video player plugin won't be build.")
  endif()
endif()

if (BUILD_GSTREAMER_AUDIO_PLAYER_PLUGIN)
  if (TRY_BUILD_GSTREAMER_AUDIO_PLAYER_PLUGIN)
    pkg_check_modules(LIBGSTREAMER IMPORTED_TARGET gstreamer-1.0)
    pkg_check_modules(LIBGSTREAMER_APP IMPORTED_TARGET gstreamer-app-1.0)
    pkg_check_modules(LIBGSTREAMER_AUDIO IMPORTED_TARGET gstreamer-audio-1.0)
  else()
    pkg_check_modules(LIBGSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0)
    pkg_check_modules(LIBGSTREAMER_APP REQUIRED IMPORTED_TARGET gstreamer-app-1.0)
    pkg_check_modules(LIBGSTREAMER_AUDIO REQUIRED IMPORTED_TARGET gstreamer-audio-1.0)
  endif()

  if (LIBGSTREAMER_FOUND AND LIBGSTREAMER_APP_FOUND AND LIBGSTREAMER_AUDIO_FOUND)
    target_sources(flutterpi_module PRIVATE
      src/plugins/audioplayers/plugin.c
      src/plugins/audioplayers/player.c
    )
    target_link_libraries(flutterpi_module PUBLIC
      PkgConfig::LIBGSTREAMER
      PkgConfig::LIBGSTREAMER_APP
      PkgConfig::LIBGSTREAMER_AUDIO
    )
  else()
    message(NOTICE "Couldn't find gstreamer libraries. Gstreamer audio player plugin won't be build.")
  endif()
endif()

# Needed so dart VM can actually resolve symbols in the same
# executable.
target_link_options(flutterpi_module PRIVATE
  -rdynamic
)

if (ENABLE_TSAN)
  target_link_options(flutterpi_module PRIVATE -fsanitize=thread)
  target_compile_options(flutterpi_module PRIVATE -fsanitize=thread)
endif()
if (ENABLE_ASAN)
  # when we use asan, we need to force linking against the C++ stdlib.
  # If we don't link against it, and load a dynamic library that's linked against the C++ stdlib (like the flutter engine),
  # and something in the dynamically loaded library triggers asan, it'll throw an error because it hasn't
  # intercepted stdc++ yet.
  # Also disable --as-needed so we _actually_ link against c++, even though we don't use any symbols from it.
  target_link_libraries(flutterpi_module stdc++)
  target_link_options(flutterpi_module PRIVATE -fsanitize=address -fno-omit-frame-pointer -Wl,--no-as-needed)
  target_compile_options(flutterpi_module PRIVATE -fsanitize=address)

  check_c_compiler_flag(-static-libasan HAS_STATIC_LIBASAN)
  if (HAS_STATIC_LIBASAN)
    target_link_options(flutterpi_module PRIVATE -static-libasan)
  endif()
endif()
if (ENABLE_UBSAN)
  target_link_options(flutterpi_module PRIVATE -fsanitize=undefined)
  target_compile_options(flutterpi_module PRIVATE -fsanitize=undefined)
endif()
if (ENABLE_MTRACE)
  target_compile_definitions(flutterpi_module PRIVATE "ENABLE_MTRACE")
endif()

add_executable(
  flutter-pi
  src/main.c
)

target_link_libraries(
  flutter-pi
  flutterpi_module
)

install(TARGETS flutter-pi RUNTIME DESTINATION bin)

if(ENABLE_TESTS)
  include(CTest)

  add_subdirectory(third_party)
  add_subdirectory(test)
endif()
