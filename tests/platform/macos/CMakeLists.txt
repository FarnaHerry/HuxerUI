huxerui_add_test_suite(huxerui_macos_tests HuxerUIMacOSTests
        LABELS platform macos native
        LIBRARIES ${HUXERUI_TEST_LIBRARY}
                  "-framework AppKit" "-framework Foundation" "-framework CoreGraphics"
                  "-framework CoreVideo" "-framework Metal" "-framework UserNotifications"
        SOURCES "${HUXERUI_TEST_DIR}/support/builtin_resources.cpp"
)
target_sources(huxerui_macos_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/examples/platform_module/timer.cpp"
        "${HUXERUI_PROJECT_DIR}/examples/platform_module/apple_timer.mm"
        local_notification.mm
        http.mm
        application.mm
        platform_module.mm
        platform_bridge.mm
        platform_view.mm
        external_texture.mm
        renderer.mm
        file_picker.mm
        text_input.mm
        window_chrome.cpp
)
set_source_files_properties(
        "${HUXERUI_PROJECT_DIR}/examples/platform_module/apple_timer.mm"
        platform_bridge.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc"
)
target_include_directories(huxerui_macos_tests PRIVATE
        "${HUXERUI_PROJECT_DIR}/examples/platform_module"
        "${HUXERUI_PROJECT_DIR}/platform/macos"
)
if (TARGET ${HUXERUI_STATIC_LIB_NAME})
    add_test(NAME HuxerUIMacStaticPlatformBridgeImport
            COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/check_platform_bridge.sh"
                    "${HUXERUI_PROJECT_DIR}"
                    "${HUXERUI_PUBLIC_INCLUDE_DIR}"
                    "$<TARGET_FILE:${HUXERUI_STATIC_LIB_NAME}>"
                    "${CMAKE_CURRENT_BINARY_DIR}/macos_platform_bridge_static"
    )
endif ()
if (TARGET ${HUXERUI_SHARED_LIB_NAME})
    add_test(NAME HuxerUIMacSharedPlatformBridgeImport
            COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/check_platform_bridge.sh"
                    "${HUXERUI_PROJECT_DIR}"
                    "${HUXERUI_PUBLIC_INCLUDE_DIR}"
                    "$<TARGET_FILE:${HUXERUI_SHARED_LIB_NAME}>"
                    "${CMAKE_CURRENT_BINARY_DIR}/macos_platform_bridge_shared"
    )
endif ()
target_compile_definitions(huxerui_macos_tests PRIVATE
        HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE="${HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE}"
)
foreach (bridge_test IN ITEMS HuxerUIMacStaticPlatformBridgeImport HuxerUIMacSharedPlatformBridgeImport)
    if (TEST ${bridge_test})
        set_tests_properties(${bridge_test} PROPERTIES LABELS "platform;macos;native;headers")
    endif ()
endforeach ()

# AppKit owns a process-wide NSApplication; each native scenario gets a fresh process and bundle identity.
add_executable(huxerui_macos_native_tests
        "${HUXERUI_TEST_DIR}/tests_main.cpp"
        multi_window_native.mm
)
target_compile_features(huxerui_macos_native_tests PRIVATE cxx_std_20)
target_compile_options(huxerui_macos_native_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(huxerui_macos_native_tests PRIVATE
        HuxerUI3p::Catch2 ${HUXERUI_TEST_LIBRARY} "-framework AppKit" "-framework Foundation"
)
set_source_files_properties(multi_window_native.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
set_target_properties(huxerui_macos_native_tests PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.huxerui.tests.multi-window"
)
_huxerui_append_resource_package_input(huxerui_macos_native_tests "${HUXERUI_TEST_BUILTIN_RESOURCE_PACKAGE}")

foreach (native_case IN ITEMS
        WindowFrameFailuresPropagateFromTheSharedLoop
        WindowCloseWaitsForKeyDispatchToReturn
        ApplicationModalsStackAndCoverNewWindows
        RetirementKeepsWindowlessRuntimeAliveAndCanReopen
)
    add_test(NAME "HuxerUIMacOSNative${native_case}"
            COMMAND huxerui_macos_native_tests "MacNative${native_case}")
    set_tests_properties("HuxerUIMacOSNative${native_case}" PROPERTIES
            LABELS "platform;macos;native" TIMEOUT 45 PASS_REGULAR_EXPRESSION "All tests passed")
endforeach ()

set(native_initial_sections
        "The initial handle requests application shutdown"
        "Native application Quit returns after orderly shutdown"
        "Native initial-window close requests application shutdown"
        "Native initial-window destruction requests application shutdown"
)
set(native_initial_names Handle Quit Close Destroy)
foreach (index RANGE 0 3)
    list(GET native_initial_sections ${index} section)
    list(GET native_initial_names ${index} name)
    add_test(NAME "HuxerUIMacOSNativeInitial${name}"
            COMMAND huxerui_macos_native_tests "MacNativeWindowsIsolateStateAndReleaseClosedAttachments"
                    --section "${section}")
    set_tests_properties("HuxerUIMacOSNativeInitial${name}" PROPERTIES
            LABELS "platform;macos;native" TIMEOUT 45 PASS_REGULAR_EXPRESSION "All tests passed")
endforeach ()

set(native_owned_sections "Close retires the owned chain" "Native destruction retires the owned chain")
set(native_owned_names Close Destroy)
foreach (index RANGE 0 1)
    list(GET native_owned_sections ${index} section)
    list(GET native_owned_names ${index} name)
    add_test(NAME "HuxerUIMacOSNativeOwned${name}"
            COMMAND huxerui_macos_native_tests "MacNativeOwnedModalsPreserveIndependentWindowsAndCascade"
                    --section "${section}")
    set_tests_properties("HuxerUIMacOSNativeOwned${name}" PROPERTIES
            LABELS "platform;macos;native" TIMEOUT 45 PASS_REGULAR_EXPRESSION "All tests passed")
endforeach ()
