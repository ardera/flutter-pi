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

# configure options
option(BUILD_TEXT_INPUT_PLUGIN "Include the text input plugin in the finished binary. Enables text input (to flutter text fields, for example) via attached keyboards." ON)
option(BUILD_RAW_KEYBOARD_PLUGIN "Include the raw keyboard plugin in the finished binary. Enables raw keycode listening in flutter via the flutter RawKeyboard interface." ON)
option(BUILD_TEST_PLUGIN "Include the test plugin in the finished binary. Allows testing platform channel communication." OFF)
option(BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN "Include the gstreamer based video plugins in the finished binary. Allows for more stable, hardware accelerated video playback in flutter using gstreamer." ON)
option(BUILD_GSTREAMER_AUDIO_PLAYER_PLUGIN "Include the gstreamer based audio plugins in the finished binary." ON)
option(TRY_BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN "Don't throw an error if the gstreamer libs aren't found, instead just don't build the gstreamer video player plugin in that case." ON)
option(TRY_BUILD_GSTREAMER_AUDIO_PLAYER_PLUGIN "Don't throw an error if the gstreamer libs aren't found, instead just don't build gstreamer audio plugin." ON)
option(ENABLE_OPENGL "Build with EGL/OpenGL rendering support." ON)
option(TRY_ENABLE_OPENGL "Don't throw an error if EGL/OpenGL aren't found, instead just build without EGL/OpenGL support in that case." ON)
option(ENABLE_VULKAN "Build with Vulkan rendering support." OFF)
option(TRY_ENABLE_VULKAN "Don't throw an error if vulkan isn't found, instead just build without vulkan support in that case." OFF)
set(VULKAN_DEBUG "AUTO" CACHE STRING "Enable vulkan validation layers and verbose vulkan logging. (ON/OFF/AUTO)")
set_property(CACHE VULKAN_DEBUG PROPERTY STRINGS ON OFF AUTO)
option(ENABLE_SOFTWARE "Build with software rendering support." ON)
option(DUMP_ENGINE_LAYERS "True if flutter-pi should dump the list of rendering layers that the flutter engine sends to flutter-pi on each draw." OFF)
option(WARN_MISSING_FIELD_INITIALIZERS "True of the compiler should be instructed to warn about missing field initializers. This needs some hacky workarounds in the code so gcc won't report spurious warnings, so this should only be enabled if the warnings are explicitly required." OFF)
option(ENABLE_TSAN "True to build & link with -fsanitize=thread" OFF)
option(ENABLE_ASAN "True to build & link with -fsanitize=address" OFF)
option(ENABLE_UBSAN "True to build & link with -fsanitize=undefined" OFF)
option(ENABLE_MTRACE "True if flutter-pi should call GNU mtrace() on startup." OFF)
option(ENABLE_TESTS "True if tests should be built. Requires Unity to be checked out at third_party/Unity." OFF)
option(ENABLE_SESSION_SWITCHING "True if flutter-pi should be built with session switching support. Requires libseat-dev to be installed." ON)
option(TRY_ENABLE_SESSION_SWITCHING "Don't throw an error if libseat isn't found, instead just build without session switching support in that case." ON)
option(LTO "Check for IPO/LTO support and enable, if supported. May require gold/lld when building with clang. (Either using `-fuse-ld` in CMAKE_C_FLAGS or by setting as the default system linker.) Only applies to Release or RelWithDebInfo build types." ON)

set(FILESYSTEM_LAYOUTS default meta-flutter)
set(FILESYSTEM_LAYOUT "default" CACHE STRING "Where to look for the icudtl.dat, app.so/libapp.so, flutter asset bundle.")
set_property(CACHE FILESYSTEM_LAYOUT PROPERTY STRINGS ${FILESYSTEM_LAYOUTS})

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Download the flutter_embedder.h if it's not specified
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

# Those libraries we definitely need.
include(FindPkgConfig)
pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm)
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
  src/modesetting.c
  src/collection.c
  src/cursor.c
  src/keyboard.c
  src/user_input.c
  src/locales.c
  src/notifier_listener.c
  src/pixel_format.c
  src/filesystem_layout.c
  src/compositor_ng.c
  src/surface.c
  src/render_surface.c
  src/tracer.c
  src/dmabuf_surface.c
  src/frame_scheduler.c
  src/window.c
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
)

target_compile_options(flutterpi_module PRIVATE
  $<$<CONFIG:Debug>:-O0 -Wall -Wextra -Wno-sign-compare -Werror -ggdb -U_FORTIFY_SOURCE -DDEBUG>
  $<$<CONFIG:RelWithDebInfo>:-O3 -Wall -Wextra -Wno-sign-compare -ggdb>
  $<$<CONFIG:Release>:-O3 -Wall -Wextra -Wno-sign-compare>
)

# GCC prior to 11.3 reports false positives for missing-field-initializers warning.
if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
  if (CMAKE_C_COMPILER_VERSION VERSION_LESS "11.3")
    target_compile_options(flutterpi_module PRIVATE -Wno-missing-field-initializers)
  endif()
endif()

# libinput stuff
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
target_compile_definitions(flutterpi_module PRIVATE HAS_KMS HAS_GBM HAS_FBDEV)

pkg_check_modules(EGL IMPORTED_TARGET egl)
pkg_check_modules(GLESV2 IMPORTED_TARGET glesv2)

if (ENABLE_OPENGL)
  if (EGL_FOUND AND GLESV2_FOUND)
    target_sources(flutterpi_module PRIVATE
      src/egl_gbm_render_surface.c
      src/gl_renderer.c
    )
    target_link_libraries(flutterpi_module PUBLIC
      PkgConfig::EGL
      PkgConfig::GLESV2
    )
    target_compile_definitions(flutterpi_module PRIVATE HAS_EGL HAS_GL)
  elseif (TRY_ENABLE_OPENGL)
    message("EGL and/or OpenGL was not found. Flutter-pi will build without EGL/OpenGL rendering support.")
  else()
    message(SEND_ERROR "EGL and/or OpenGL was not found. Try building with `-DTRY_ENABLE_OPENGL=On` if you want to just disable EGL/OpenGL support in that case.")
  endif()
endif()

if (EGL_FOUND)
  target_include_directories(flutterpi_module PRIVATE ${EGL_INCLUDE_DIRS})
  target_compile_options(flutterpi_module PRIVATE ${EGL_CFLAGS})
else()
  FetchContent_Declare(
    EGL_REGISTRY
    GIT_REPOSITORY "https://github.com/KhronosGroup/EGL-Registry"
    GIT_TAG "main"
  )

  # Don't use FetchContent_MakeAvailable here to be compatible with cmake < 3.14 (debian buster)
  FetchContent_GetProperties(egl_registry)
  if(NOT egl_registry_POPULATED)
    FetchContent_Populate(egl_registry)
  endif()

  target_include_directories(flutterpi_module PRIVATE ${egl_registry_SOURCE_DIR}/api/)
endif()

if (GLESV2_FOUND)
  target_include_directories(flutterpi_module PRIVATE ${GLESV2_INCLUDE_DIRS})
  target_compile_options(flutterpi_module PRIVATE ${GLESV2_CFLAGS})
else()
  FetchContent_Declare(
    OGL_REGISTRY
    GIT_REPOSITORY "https://github.com/KhronosGroup/OpenGL-Registry"
    GIT_TAG "main"
  )

  # Don't use FetchContent_MakeAvailable here to be compatible with cmake < 3.14 (debian buster)
  FetchContent_GetProperties(ogl_registry)
  if(NOT ogl_registry_POPULATED)
    FetchContent_Populate(ogl_registry)
  endif()
  
  target_include_directories(flutterpi_module PRIVATE ${ogl_registry_SOURCE_DIR}/api/)
endif()

# Filesystem Layout
# meta-flutter or normal flutter-pi.
# see src/filesystem_layout.c for details.
if(NOT FILESYSTEM_LAYOUT IN_LIST FILESYSTEM_LAYOUTS)
  message(FATAL_ERROR "FILESYSTEM_LAYOUT must be one of ${FILESYSTEM_LAYOUTS}")
endif()

message(STATUS "Filesystem Layout ...... ${FILESYSTEM_LAYOUT}")
if(FILESYSTEM_LAYOUT STREQUAL default)
  target_compile_definitions(flutterpi_module PRIVATE "FILESYSTEM_LAYOUT_DEFAULT")
elseif(FILESYSTEM_LAYOUT STREQUAL meta-flutter)
  target_compile_definitions(flutterpi_module PRIVATE "FILESYSTEM_LAYOUT_METAFLUTTER")
endif()

pkg_check_modules(VULKAN IMPORTED_TARGET vulkan)
if (ENABLE_VULKAN)
  if (VULKAN_FOUND)
    target_sources(flutterpi_module PRIVATE
      src/vk_gbm_render_surface.c
      src/vk_renderer.c
    )
    target_link_libraries(flutterpi_module PUBLIC
      PkgConfig::VULKAN
    )
    target_compile_definitions(flutterpi_module PRIVATE HAS_VULKAN)
  elseif (TRY_ENABLE_VULKAN)
    message("Vulkan was not found. Flutter-pi will build without vulkan rendering support.")
  else()
    message(SEND_ERROR "Vulkan was not found. Try building with `-DTRY_ENABLE_VULKAN=On` if you want to just disable vulkan support in that case.")
  endif()
endif()

# We still need the headers for the types, so fetch the headers and use them.
if (NOT VULKAN_FOUND)
  message("Fetching vulkan headers.")
  FetchContent_Declare(
    vulkan_headers
    GIT_REPOSITORY "https://github.com/KhronosGroup/Vulkan-Headers"
    GIT_TAG "main"
  )

  # Don't use FetchContent_MakeAvailable here to be compatible with cmake < 3.14 (debian buster)
  FetchContent_GetProperties(vulkan_headers)
  if(NOT vulkan_headers_POPULATED)
    FetchContent_Populate(vulkan_headers)
  endif()

  target_include_directories(flutterpi_module PRIVATE ${vulkan_headers_SOURCE_DIR}/include/)
endif()

# Session switching support (using libseat)
message(STATUS "Session switching ...... ${ENABLE_SESSION_SWITCHING}")
if (ENABLE_SESSION_SWITCHING)
  if (TRY_ENABLE_SESSION_SWITCHING)
    pkg_check_modules(LIBSEAT IMPORTED_TARGET libseat)
  else()
    pkg_check_modules(LIBSEAT REQUIRED IMPORTED_TARGET libseat)
  endif()

  if (LIBSEAT_FOUND)
    target_link_libraries(flutterpi_module PUBLIC PkgConfig::LIBSEAT)
    target_compile_definitions(flutterpi_module PUBLIC "HAS_LIBSEAT")
  else()
    message("libseat was not found. flutter-pi will be built without session switching support.")
  endif()
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
# executable. (For dart:ffi DynamicLibrary.executable / DynamicLibrary.process)
target_link_options(flutterpi_module PRIVATE -rdynamic)

# Define VULKAN_DEBUG if it was set to On, or if it was set to AUTO and we're using debug mode.
if (VULKAN_DEBUG STREQUAL "AUTO")
  target_compile_definitions(flutterpi_module PRIVATE $<$<CONFIG:Debug>:VULKAN_DEBUG>)
elseif (VULKAN_DEBUG)
  target_compile_definitions(flutterpi_module PRIVATE VULKAN_DEBUG)
endif()

# Some sanitizer configs.
if (ENABLE_TSAN)
  target_link_options(flutterpi_module PUBLIC -fsanitize=thread)
  target_compile_options(flutterpi_module PUBLIC -fsanitize=thread)
endif()
if (ENABLE_ASAN)
  # when we use asan, we need to force linking against the C++ stdlib.
  # If we don't link against it, and load a dynamic library that's linked against the C++ stdlib (like the flutter engine),
  # and something in the dynamically loaded library triggers asan, it'll throw an error because it hasn't
  # intercepted stdc++ yet.
  # Also disable --as-needed so we _actually_ link against c++, even though we don't use any symbols from it.
  target_link_libraries(flutterpi_module stdc++)
  target_link_options(flutterpi_module PUBLIC -fsanitize=address -fno-omit-frame-pointer -Wl,--no-as-needed)
  target_compile_options(flutterpi_module PUBLIC -fsanitize=address)

  check_c_compiler_flag(-static-libasan HAS_STATIC_LIBASAN)
  if (HAS_STATIC_LIBASAN)
    target_link_options(flutterpi_module PUBLIC -static-libasan)
  endif()
endif()
if (ENABLE_UBSAN)
  target_link_options(flutterpi_module PUBLIC -fsanitize=undefined)
  target_compile_options(flutterpi_module PUBLIC -fsanitize=undefined)
endif()
if (ENABLE_MTRACE)
  target_compile_definitions(flutterpi_module PUBLIC "ENABLE_MTRACE")
endif()

# Actual flutter-pi executable.
add_executable(
  flutter-pi
  src/main.c
)
target_link_libraries(
  flutter-pi
  flutterpi_module
)
install(TARGETS flutter-pi RUNTIME DESTINATION bin)

# Enable lto if supported.
cmake_policy(SET CMP0069 NEW)
include(CheckIPOSupported)
# include(CheckLinkerFlag)

set(USE_LTO OFF)
# set(NEEDS_GOLD OFF)
# set(NEEDS_LLD OFF)
if(LTO AND (CMAKE_BUILD_TYPE STREQUAL Release OR CMAKE_BUILD_TYPE STREQUAL RelWithDebInfo))
  # So people can specify `-fuse-ld=lld` in the CMAKE_C_FLAGS.
  # Otherwise check_ipo_supported will not use CMAKE_C_FLAGS.
  if (POLICY CMP0138)
    cmake_policy(SET CMP0138 NEW)
  endif()

  check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_SUPPORT_OUTPUT)
  
  # Try to enable IPO with gold and lld.
  # Needs CMP0138.
  # (untested because CMP0138 required CMake 3.24, that's why it's commented out)
  # if (NOT IPO_SUPPORTED)
  #   check_linker_flag(C "-fuse-ld=gold" SUPPORTS_GOLD)
  #   if (SUPPORTS_GOLD)
  #     set(OLD_CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
  #     
  #     set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fuse-ld=gold")
  #     try_compile()
  #     check_ipo_supported(RESULT IPO_SUPPORTED_WITH_GOLD OUTPUT IPO_SUPPORT_OUTPUT)
  #     if (IPO_SUPPORTED_WITH_GOLD)
  #       set(IPO_SUPPORTED ON)
  #       set(NEEDS_GOLD ON)
  #     endif()
  #     
  #     set(CMAKE_C_FLAGS "${OLD_CMAKE_C_FLAGS}")
  #   endif()
  #
  #   check_linker_flag(C "-fuse-ld=lld" SUPPORTS_LLD)
  #   if (SUPPORTS_LLD)
  #     set(OLD_CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
  #     
  #     set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fuse-ld=lld")
  #     check_ipo_supported(RESULT IPO_SUPPORTED_WITH_LLD OUTPUT IPO_SUPPORT_OUTPUT)
  #     if (IPO_SUPPORTED_WITH_LLD)
  #       set(IPO_SUPPORTED ON)
  #       set(NEEDS_LLD ON)
  #     endif()
  #     
  #     set(CMAKE_C_FLAGS "${OLD_CMAKE_C_FLAGS}")
  #   endif()
  # endif()
  
  if (IPO_SUPPORTED)
    set(USE_LTO ON)
  else()
    message(WARNING "IPO/LTO was requested in the configure options, but is not supported by the toolchain. Check CMakeFiles/CMakeError.log for details.")
  endif()
endif()

message(STATUS "IPO/LTO ................ ${USE_LTO}")
if (USE_LTO)
  set_property(TARGET flutterpi_module PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  set_property(TARGET flutter-pi PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  # if (NEEDS_GOLD)
  #   Technically specifying only for one would suffice.
  #   target_link_options(flutterpi_module PUBLIC "-fuse-ld=gold")
  #   target_link_options(flutter-pi PUBLIC "-fuse-ld=gold")
  # elseif (NEEDS_LLD)
  #   target_link_options(flutterpi_module PUBLIC "-fuse-ld=lld")
  #   target_link_options(flutter-pi PUBLIC "-fuse-ld=lld")
  # endif()
endif()

if(ENABLE_TESTS)
  include(CTest)

  add_subdirectory(third_party)
  add_subdirectory(test)
endif()
