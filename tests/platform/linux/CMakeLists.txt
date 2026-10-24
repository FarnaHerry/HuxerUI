huxerui_add_test_suite(huxerui_linux_tests HuxerUILinuxTests
        LABELS platform linux native
        LIBRARIES ${HUXERUI_TEST_LIBRARY}
        MAIN "${CMAKE_CURRENT_SOURCE_DIR}/tests_main.cpp"
        SOURCES "${HUXERUI_TEST_DIR}/support/builtin_resources.cpp"
)
target_sources(huxerui_linux_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/examples/platform_module/linux_timer.cpp"
        "${HUXERUI_PROJECT_DIR}/examples/platform_module/timer.cpp"
        external_texture.cpp
        file.cpp
        file_picker.cpp
        floating_window_native.cpp
        gl_effects.cpp
        http.cpp
        multi_window_native.cpp
        platform_module.cpp
        renderer.cpp
        resident_effects.cpp
        text_input.cpp
        window_chrome.cpp
        window_drag_native.cpp
)
target_include_directories(huxerui_linux_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/examples/platform_module"
        "$<TARGET_PROPERTY:huxerui_core_objects,INCLUDE_DIRECTORIES>"
)
target_link_libraries(huxerui_linux_tests PRIVATE
        ${CMAKE_DL_LIBS}
        PkgConfig::HUXERUI_GTK4
        PkgConfig::HUXERUI_EPOXY
        PkgConfig::HUXERUI_GIO
)
target_include_directories(huxerui_linux_tests PRIVATE "${HUXERUI_PROJECT_DIR}/platform/linux")
target_compile_definitions(huxerui_linux_tests PRIVATE
        HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE="${HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE}"
)
_huxerui_append_resource_package_input(huxerui_linux_tests "${HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE}")
