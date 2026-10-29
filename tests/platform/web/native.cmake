huxerui_add_test_suite(huxerui_web_tests HuxerUIWebTests
        LABELS platform web native
        LIBRARIES ${HUXERUI_TEST_LIBRARY}
        SOURCES "${HUXERUI_TEST_DIR}/support/builtin_resources.cpp"
)
target_sources(huxerui_web_tests PRIVATE
        external_texture.cpp
        test_entry.cpp
        text_layout.cpp
)
target_include_directories(huxerui_web_tests PRIVATE "${HUXERUI_PROJECT_DIR}/platform/web")
target_compile_definitions(huxerui_web_tests PRIVATE
        HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE="${HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE}"
)

add_test(NAME HuxerUIWebTestFailureExitTests
        COMMAND "${CMAKE_COMMAND}"
                "-DTEST_EXECUTABLE=$<TARGET_FILE:huxerui_web_tests>"
                "-DTEST_EMULATOR=$<TARGET_PROPERTY:huxerui_web_tests,CROSSCOMPILING_EMULATOR>"
                -P "${CMAKE_CURRENT_LIST_DIR}/test_entry.cmake"
)
set_tests_properties(HuxerUIWebTestFailureExitTests PROPERTIES
        LABELS "build;platform;web"
)
