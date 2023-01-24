add_executable(platformchannel_test
    platformchannel_test.c
)

target_link_libraries(platformchannel_test
    flutterpi_module
    Unity
)

add_test(platformchannel_test platformchannel_test)