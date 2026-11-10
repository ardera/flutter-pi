add_executable(platformchannel_test
    platformchannel_test.c
)

target_link_libraries(
    platformchannel_test
    flutterpi_module
    Unity
)

add_test(platformchannel_test platformchannel_test)

# Exercise the video plugin without opening DRM, EGL or a display. Each test
# includes one implementation to inspect private lifetime state; link the rest
# with the same module configuration and mock only external boundaries.
if (BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN AND HAVE_EGL_GLES2 AND LIBGSTREAMER_VIDEO_FOUND AND LIBGSTREAMER_ALLOCATORS_FOUND)
    function(add_video_test name implementation)
        get_target_property(video_sources flutterpi_module SOURCES)
        list(FILTER video_sources EXCLUDE REGEX "src/plugins/gstreamer_video_player/${implementation}\\.c$")
        list(TRANSFORM video_sources PREPEND "${PROJECT_SOURCE_DIR}/")
        add_executable(${name} ${name}.c ${video_sources})
        target_include_directories(${name} PRIVATE $<TARGET_PROPERTY:flutterpi_module,INCLUDE_DIRECTORIES>)
        target_compile_options(${name} PRIVATE $<TARGET_PROPERTY:flutterpi_module,COMPILE_OPTIONS>)
        target_compile_definitions(${name} PRIVATE $<TARGET_PROPERTY:flutterpi_module,COMPILE_DEFINITIONS>)
        target_link_libraries(${name} PRIVATE $<TARGET_PROPERTY:flutterpi_module,LINK_LIBRARIES> Unity)
        target_link_options(${name} PRIVATE $<TARGET_PROPERTY:flutterpi_module,LINK_OPTIONS>)
        foreach(symbol IN LISTS ARGN)
            target_link_options(${name} PRIVATE "-Wl,--wrap=${symbol}")
        endforeach()
        add_test(${name} ${name})
        set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    endfunction()
    add_video_test(gstreamer_frame_test frame
        gbm_bo_create gbm_bo_map gbm_bo_unmap gbm_bo_get_fd gbm_bo_destroy
        eglMakeCurrent glGenTextures glBindTexture glGetError glDeleteTextures)
    add_video_test(gstreamer_player_test player
        gst_element_set_state gst_element_seek notifier_deinit frame_interface_unref texture_destroy)
    add_video_test(gstreamer_plugin_test plugin
        gstplayer_get_userdata_locked gstplayer_get_video_info_notifier gstplayer_get_buffering_state_notifier gstplayer_destroy
        plugin_registry_set_receiver plugin_registry_set_receiver_locked plugin_registry_remove_receiver plugin_registry_remove_receiver_locked
        platch_respond_success_std platch_respond_not_implemented platch_send_success_event_std)
endif()

add_executable(notifier_test notifier_test.c)
target_link_libraries(notifier_test flutterpi_module Unity)
add_test(notifier_test notifier_test)
set_tests_properties(notifier_test PROPERTIES TIMEOUT 30)


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
