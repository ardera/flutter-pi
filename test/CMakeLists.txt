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

# This test includes modesetting.c with a mocked DRM submission boundary.
add_executable(kms_present_test kms_present_test.c)
target_include_directories(kms_present_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/flutter_embedder_header/include
    ${CMAKE_BINARY_DIR}
)
target_compile_options(kms_present_test PRIVATE -UNDEBUG -ffunction-sections -fdata-sections)
target_link_options(kms_present_test PRIVATE -Wl,--gc-sections)
target_link_libraries(kms_present_test PRIVATE PkgConfig::DRM PkgConfig::GBM Threads::Threads m)
add_test(kms_present_test kms_present_test)
