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

if(NOT FLUTTER_ENGINE_LIBRARY)

  include(FetchContent)

  if(NOT FLUTTER_ENGINE_SHA)

    if(NOT CHANNEL)
        set(CHANNEL "stable" CACHE STRING "Choose the channel, options are: master, dev, beta, stable" FORCE)
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

  # Download and setup the Flutter Engine.

  set(FLUTTER_EMBEDDER_ARTIFACTS_ZIP ${CMAKE_BINARY_DIR}/flutter_embedder_${FLUTTER_ENGINE_SHA}.zip)
  set(FLUTTER_BUCKET_BASE "https://storage.googleapis.com/flutter_infra/flutter")

  if(NOT EXISTS ${FLUTTER_EMBEDDER_ARTIFACTS_ZIP})
    file(DOWNLOAD
      ${FLUTTER_BUCKET_BASE}/${FLUTTER_ENGINE_SHA}/linux-x64/linux-x64-embedder
      ${FLUTTER_EMBEDDER_ARTIFACTS_ZIP}
      SHOW_PROGRESS
    )
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E tar xzf ${FLUTTER_EMBEDDER_ARTIFACTS_ZIP}
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
  endif()

  set(FLUTTER_ENGINE_LIBRARY ${CMAKE_BINARY_DIR}/libflutter_engine.so)
else()
  message(STATUS "Engine ................. ${FLUTTER_ENGINE_LIBRARY}")
endif()

include(ExternalProject)

set(ENV{PKG_CONFIG_PATH} ${PKG_CONFIG_PATH})
message(STATUS "PKG_CONFIG_PATH ........ $ENV{PKG_CONFIG_PATH}")

include(FindPkgConfig)
pkg_check_modules(GBM REQUIRED gbm)
pkg_check_modules(DRM REQUIRED libdrm)
pkg_check_modules(GLESV2 REQUIRED glesv2)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(LIBSYSTEMD REQUIRED libsystemd)
pkg_check_modules(LIBUDEV libudev)
pkg_check_modules(GPIOD libgpiod)

set(FLUTTER_PI_SRC
  src/flutter-pi.c
	src/platformchannel.c
	src/pluginregistry.c
	src/console_keyboard.c
	src/texture_registry.c
	src/compositor.c
	src/modesetting.c
	src/collection.c
	src/cursor.c
	src/plugins/services.c
	src/plugins/testplugin.c
	src/plugins/text_input.c
	src/plugins/raw_keyboard.c
	src/plugins/spidev.c
	src/plugins/omxplayer_video_player.c
)

if(GPIOD_FOUND)
  list(APPEND FLUTTER_PI_SRC src/plugins/gpiod.c)
  add_compile_options(-DBUILD_GPIOD_PLUGIN)
else()
  message(STATUS "Could not find gpiod library and development headers. flutter-pi will be built without gpiod support. To install, execute 'sudo apt install libgpiod-dev'")
endif()

if (NOT LIBUDEV_FOUND)
  message(STATUS "Could not find libudev.so and libudev development headers. flutter-pi will be built without udev (hotplugging) support. To install, execute 'sudo apt install libudev-dev'")
  add_compile_options(-DBUILD_WITHOUT_UDEV_SUPPORT)
endif()

add_executable(flutter-pi ${FLUTTER_PI_SRC})

target_link_libraries(flutter-pi
  ${GPIOD_LDFLAGS} ${GBM_LDFLAGS}
  ${DRM_LDFLAGS} ${GLESV2_LDFLAGS} ${EGL_LDFLAGS}
  pthread dl
)

target_include_directories(flutter-pi PRIVATE
  ${CMAKE_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/include/plugins
  ${GBM_INCLUDE_DIRS} ${DRM_INCLUDE_DIRS}
  ${GLESV2_INCLUDE_DIRS} ${EGL_INCLUDE_DIRS}
)

target_compile_options(flutter-pi PRIVATE
  ${GBM_CFLAGS} ${DRM_CFLAGS}
  ${GLESV2_CFLAGS} ${EGL_CFLAGS}
  ${GPIOD_CFLAGS} -ggdb
  -DBUILD_TEXT_INPUT_PLUGIN
	-DBUILD_SPIDEV_PLUGIN
	-DBUILD_TEST_PLUGIN
	-DBUILD_OMXPLAYER_VIDEO_PLAYER_PLUGIN
)

target_link_options(flutter-pi PRIVATE
  -rdynamic
)

install(TARGETS flutter-pi RUNTIME DESTINATION bin)
