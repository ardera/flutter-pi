add_executable(platformchannel_test
    platformchannel_test.c
)

target_link_libraries(
    platformchannel_test
    flutterpi_module
    Unity
)

add_test(platformchannel_test platformchannel_test)

add_executable(flutterpi_test
    flutterpi_test.c
)

target_link_libraries(
    flutterpi_test
    flutterpi_module
    Unity
)

add_test(flutterpi_test flutterpi_test)

if (BUILD_WEBVIEW_CEF_PLUGIN)
    # flutterpi_module carries C++ objects (the CEF bridge) then, and both test
    # binaries are C, so CMake would pick the C driver to link them and leave out
    # the C++ runtime.
    set_target_properties(platformchannel_test flutterpi_test PROPERTIES LINKER_LANGUAGE CXX)
endif()