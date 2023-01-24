add_executable(testsuite_app
    platformchannel_test.c
)

target_link_libraries(testsuite_app
    flutterpi_module
    Unity
)

add_test(testsuite_test testsuite_app)