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

project(flutter-pi LANGUAGES C VERSION "1.0.0")

message(STATUS "Generator .............. ${CMAKE_GENERATOR}")
message(STATUS "Build Type ............. ${CMAKE_BUILD_TYPE}")

option(BUILD_TEXT_INPUT_PLUGIN "Include the text input plugin in the finished binary. Enables text input (to flutter text fields, for example) via attached keyboards." ON)
option(BUILD_RAW_KEYBOARD_PLUGIN "Include the raw keyboard plugin in the finished binary. Enables raw keycode listening in flutter via the flutter RawKeyboard interface." ON)
option(BUILD_TEST_PLUGIN "Include the test plugin in the finished binary. Allows testing platform channel communication." OFF)
option(BUILD_OMXPLAYER_VIDEO_PLAYER_PLUGIN "Include the omxplayer_video_player plugin in the finished binary. Allows for hardware accelerated video playback in flutter using omxplayer." OFF)
option(BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN "Include the gstreamer_video_player plugin in the finished binary. Allows for more stable, hardware accelerated video playback in flutter using gstreamer." ON)
option(OMXPLAYER_SUPPORTS_RUNTIME_ROTATION "Whether omxplayer supports runtime rotation." OFF)
option(DUMP_ENGINE_LAYERS "True if flutter-pi should dump the list of rendering layers that the flutter engine sends to flutter-pi on each draw." OFF)

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

set(FLUTTER_PI_SRC
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
  src/event_loop.c
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
if (BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN)
  list(APPEND FLUTTER_PI_SRC
    src/plugins/gstreamer_video_player/plugin.c
    src/plugins/gstreamer_video_player/player.c
    src/plugins/gstreamer_video_player/frame.c
  )
  pkg_check_modules(LIBGSTREAMER gstreamer-1.0)
  pkg_check_modules(LIBGSTREAMER_PLUGINS_BASE gstreamer-plugins-base-1.0)
  pkg_check_modules(LIBGSTREAMER_APP gstreamer-app-1.0)
  pkg_check_modules(LIBGSTREAMER_ALLOCATORS gstreamer-allocators-1.0)
  pkg_check_modules(LIBGSTREAMER_VIDEO gstreamer-video-1.0)

  if (NOT (LIBGSTREAMER_FOUND AND LIBGSTREAMER_PLUGINS_BASE_FOUND AND LIBGSTREAMER_APP_FOUND AND LIBGSTREAMER_ALLOCATORS_FOUND AND LIBGSTREAMER_VIDEO_FOUND))
    message(STATUS "BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN is enabled, but not all gstreamer libraries have been found. gstreamer video player won't be built.")
    set(BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN OFF)
  endif()
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
  $<$<CONFIG:DEBUG>:-O0 -Wall -Wextra -Wno-unused-function -Wno-sign-compare -Wno-missing-field-initializers -Werror -ggdb -DDEBUG>
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
if (OMXPLAYER_SUPPORTS_RUNTIME_ROTATION)
  target_compile_definitions(flutter-pi PRIVATE "OMXPLAYER_SUPPORTS_RUNTIME_ROTATION")
endif()
if (BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN)
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
endif()

target_link_options(flutter-pi PRIVATE
  -rdynamic
)

install(TARGETS flutter-pi RUNTIME DESTINATION bin)
