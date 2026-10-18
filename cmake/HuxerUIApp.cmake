include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/HuxerUILibraries.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/HuxerUIRuntimeDependencies.cmake")

function(_huxerui_configure_ios_app_core target_name)
    get_property(HUXERUI_IOS_APP_FRAMEWORK_TARGET
            TARGET ${target_name}
            PROPERTY HUXERUI_APP_FRAMEWORK_TARGET
    )
    get_property(HUXERUI_IOS_LIBRARY_TARGETS
            TARGET ${target_name}
            PROPERTY HUXERUI_LIBRARIES
    )
    find_program(HUXERUI_IOS_LIBTOOL libtool REQUIRED)
    set(HUXERUI_IOS_CORE_DIRECTORY
            "${CMAKE_BINARY_DIR}/huxerui-ios/${target_name}"
    )
    set(HUXERUI_IOS_CORE_ARCHIVE
            "${HUXERUI_IOS_CORE_DIRECTORY}/lib${target_name}_huxerui.a"
    )
    set(HUXERUI_IOS_LINK_OPTIONS_FILE
            "${HUXERUI_IOS_CORE_DIRECTORY}/link.rsp"
    )
    get_target_property(HUXERUI_IOS_LINK_LIBRARIES
            ${HUXERUI_IOS_APP_FRAMEWORK_TARGET}
            INTERFACE_LINK_LIBRARIES
    )
    get_target_property(HUXERUI_IOS_LINK_OPTIONS
            ${HUXERUI_IOS_APP_FRAMEWORK_TARGET}
            INTERFACE_LINK_OPTIONS
    )
    if (NOT HUXERUI_IOS_LINK_LIBRARIES
            OR HUXERUI_IOS_LINK_LIBRARIES MATCHES "-NOTFOUND$")
        message(FATAL_ERROR
                "huxerui_add_app() requires iOS platform link arguments"
        )
    endif ()
    if (HUXERUI_IOS_LINK_OPTIONS MATCHES "-NOTFOUND$")
        set(HUXERUI_IOS_LINK_OPTIONS)
    endif ()
    set(HUXERUI_IOS_LINK_OPTIONS_CONTENT
            "-force_load\n\"${HUXERUI_IOS_CORE_ARCHIVE}\"\n"
    )
    foreach (HUXERUI_IOS_LINK_ARGUMENT IN LISTS
            HUXERUI_IOS_LINK_LIBRARIES
            HUXERUI_IOS_LINK_OPTIONS
    )
        string(APPEND HUXERUI_IOS_LINK_OPTIONS_CONTENT
                "${HUXERUI_IOS_LINK_ARGUMENT}\n"
        )
    endforeach ()
    # iOS 的 app 目标编译为静态库：它自己的 PRIVATE 链接依赖（应用引入的
    # 三方静态库与平台 flag）只记录在目标的 LINK_LIBRARIES 里，Xcode 侧
    # 拿不到，必须一并写进 link.rsp，否则宿主链接时全部符号未定义。
    # 有产物的目标写成带引号的 $<TARGET_FILE:>，由 file(GENERATE) 求值；
    # INTERFACE 目标没有产物、只承载 usage requirements，跳过；其余条目
    # （flag、条件 genex）原样透传给 file(GENERATE) 按平台求值。
    set(HUXERUI_IOS_APP_LINK_QUEUE "${target_name}")
    set(HUXERUI_IOS_APP_LINK_VISITED "")
    set(HUXERUI_IOS_APP_LINK_ENTRIES "")
    while (HUXERUI_IOS_APP_LINK_QUEUE)
        list(GET HUXERUI_IOS_APP_LINK_QUEUE 0 HUXERUI_IOS_LINK_SOURCE)
        list(REMOVE_AT HUXERUI_IOS_APP_LINK_QUEUE 0)
        list(APPEND HUXERUI_IOS_APP_LINK_VISITED "${HUXERUI_IOS_LINK_SOURCE}")
        set(HUXERUI_IOS_SOURCE_ITEMS "")
        foreach (HUXERUI_IOS_LINK_PROPERTY LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(HUXERUI_IOS_SOURCE_PROPERTY
                    "${HUXERUI_IOS_LINK_SOURCE}" "${HUXERUI_IOS_LINK_PROPERTY}"
            )
            if (HUXERUI_IOS_SOURCE_PROPERTY
                    AND NOT HUXERUI_IOS_SOURCE_PROPERTY MATCHES "-NOTFOUND$")
                list(APPEND HUXERUI_IOS_SOURCE_ITEMS ${HUXERUI_IOS_SOURCE_PROPERTY})
            endif ()
        endforeach ()
        foreach (HUXERUI_IOS_LINK_ITEM IN LISTS HUXERUI_IOS_SOURCE_ITEMS)
            if (HUXERUI_IOS_LINK_ITEM MATCHES "^\\$<LINK_ONLY:(.+)>$")
                set(HUXERUI_IOS_LINK_ITEM "${CMAKE_MATCH_1}")
            endif ()
            if (HUXERUI_IOS_LINK_ITEM MATCHES "^(\\$|-|/)")
                list(APPEND HUXERUI_IOS_APP_LINK_ENTRIES "${HUXERUI_IOS_LINK_ITEM}")
            elseif (TARGET "${HUXERUI_IOS_LINK_ITEM}")
                get_target_property(HUXERUI_IOS_ITEM_TYPE
                        "${HUXERUI_IOS_LINK_ITEM}" TYPE
                )
                set(HUXERUI_IOS_ITEM_NAME "${HUXERUI_IOS_LINK_ITEM}")
                if (NOT HUXERUI_IOS_ITEM_TYPE OR HUXERUI_IOS_ITEM_TYPE MATCHES "-NOTFOUND$")
                    # ALIAS 目标读不到 TYPE，顺着 ALIASED_TARGET 找本体。
                    get_target_property(HUXERUI_IOS_ITEM_ALIASED
                            "${HUXERUI_IOS_LINK_ITEM}" ALIASED_TARGET
                    )
                    if (HUXERUI_IOS_ITEM_ALIASED
                            AND NOT HUXERUI_IOS_ITEM_ALIASED MATCHES "-NOTFOUND$")
                        set(HUXERUI_IOS_ITEM_NAME "${HUXERUI_IOS_ITEM_ALIASED}")
                        get_target_property(HUXERUI_IOS_ITEM_TYPE
                                "${HUXERUI_IOS_ITEM_ALIASED}" TYPE
                        )
                    endif ()
                endif ()
                if (HUXERUI_IOS_ITEM_TYPE MATCHES
                        "^(STATIC_LIBRARY|SHARED_LIBRARY|UNKNOWN_LIBRARY|MODULE_LIBRARY)$")
                    list(APPEND HUXERUI_IOS_APP_LINK_ENTRIES
                            "\"$<TARGET_FILE:${HUXERUI_IOS_ITEM_NAME}>\"")
                endif ()
                if (NOT "${HUXERUI_IOS_ITEM_NAME}" IN_LIST HUXERUI_IOS_APP_LINK_VISITED)
                    list(APPEND HUXERUI_IOS_APP_LINK_QUEUE "${HUXERUI_IOS_ITEM_NAME}")
                endif ()
            elseif (NOT HUXERUI_IOS_LINK_ITEM MATCHES "NOTFOUND$")
                list(APPEND HUXERUI_IOS_APP_LINK_ENTRIES "${HUXERUI_IOS_LINK_ITEM}")
            endif ()
        endforeach ()
    endwhile ()
    foreach (HUXERUI_IOS_LINK_ARGUMENT IN LISTS HUXERUI_IOS_APP_LINK_ENTRIES)
        string(APPEND HUXERUI_IOS_LINK_OPTIONS_CONTENT
                "${HUXERUI_IOS_LINK_ARGUMENT}\n"
        )
    endforeach ()
    file(MAKE_DIRECTORY "${HUXERUI_IOS_CORE_DIRECTORY}")
    # file(GENERATE) 而不是 file(WRITE)：条目里的 $<TARGET_FILE:> 与条件
    # genex 只能在 generate 阶段按平台/产物求值。
    file(GENERATE OUTPUT "${HUXERUI_IOS_LINK_OPTIONS_FILE}"
            CONTENT "${HUXERUI_IOS_LINK_OPTIONS_CONTENT}"
    )

    set(HUXERUI_IOS_CORE_INPUTS
            "$<TARGET_FILE:${target_name}>"
            "$<TARGET_FILE:${HUXERUI_IOS_APP_FRAMEWORK_TARGET}>"
    )
    set(HUXERUI_IOS_CORE_DEPENDENCIES
            ${target_name}
            ${HUXERUI_IOS_APP_FRAMEWORK_TARGET}
    )
    foreach (HUXERUI_IOS_LIBRARY_TARGET IN LISTS HUXERUI_IOS_LIBRARY_TARGETS)
        list(APPEND HUXERUI_IOS_CORE_INPUTS
                "$<TARGET_FILE:${HUXERUI_IOS_LIBRARY_TARGET}>"
        )
        list(APPEND HUXERUI_IOS_CORE_DEPENDENCIES
                ${HUXERUI_IOS_LIBRARY_TARGET}
        )
    endforeach ()

    add_custom_command(
            OUTPUT "${HUXERUI_IOS_CORE_ARCHIVE}"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${HUXERUI_IOS_CORE_DIRECTORY}"
            COMMAND "${HUXERUI_IOS_LIBTOOL}" -static
                    -o "${HUXERUI_IOS_CORE_ARCHIVE}"
                    ${HUXERUI_IOS_CORE_INPUTS}
            DEPENDS ${HUXERUI_IOS_CORE_DEPENDENCIES}
            COMMENT "Linking HuxerUI iOS application core ${target_name}"
            VERBATIM
    )
    add_custom_target(${target_name}_huxerui_ios_core
            DEPENDS "${HUXERUI_IOS_CORE_ARCHIVE}"
    )
endfunction()

function(_huxerui_configure_scheduled_ios_app_cores)
    get_property(HUXERUI_IOS_APP_TARGETS
            DIRECTORY
            PROPERTY HUXERUI_IOS_APP_TARGETS
    )
    foreach (HUXERUI_IOS_APP_TARGET IN LISTS HUXERUI_IOS_APP_TARGETS)
        _huxerui_configure_ios_app_core(${HUXERUI_IOS_APP_TARGET})
    endforeach ()
endfunction()

function(_huxerui_schedule_ios_app_core target_name)
    get_target_property(HUXERUI_IOS_APP_SOURCE_DIRECTORY
            ${target_name}
            SOURCE_DIR
    )
    set_property(DIRECTORY "${HUXERUI_IOS_APP_SOURCE_DIRECTORY}" APPEND PROPERTY
            HUXERUI_IOS_APP_TARGETS
            "${target_name}"
    )
    get_property(HUXERUI_IOS_APP_CALLBACK_SCHEDULED
            DIRECTORY "${HUXERUI_IOS_APP_SOURCE_DIRECTORY}"
            PROPERTY HUXERUI_IOS_APP_CALLBACK_SCHEDULED
    )
    if (NOT HUXERUI_IOS_APP_CALLBACK_SCHEDULED)
        set_property(DIRECTORY "${HUXERUI_IOS_APP_SOURCE_DIRECTORY}" PROPERTY
                HUXERUI_IOS_APP_CALLBACK_SCHEDULED TRUE
        )
        cmake_language(DEFER
                DIRECTORY "${HUXERUI_IOS_APP_SOURCE_DIRECTORY}"
                CALL _huxerui_configure_scheduled_ios_app_cores
        )
    endif ()
endfunction()

function(huxerui_add_app target_name)
    cmake_parse_arguments(HUXERUI_APP
            ""
            "RESOURCE_NAMESPACE;RESOURCE_OUTPUT_DIRECTORY;BUNDLE_NAME;BUNDLE_IDENTIFIER"
            "SOURCES;RESOURCES"
            ${ARGN}
    )

    if (TARGET ${target_name})
        message(FATAL_ERROR
                "huxerui_add_app() target already exists: ${target_name}"
        )
    endif ()
    if (NOT HUXERUI_APP_SOURCES)
        message(FATAL_ERROR
                "huxerui_add_app() requires at least one source"
        )
    endif ()
    if (HUXERUI_APP_RESOURCES AND NOT HUXERUI_APP_RESOURCE_NAMESPACE)
        message(FATAL_ERROR
                "huxerui_add_app() requires RESOURCE_NAMESPACE when RESOURCES is present"
        )
    endif ()
    list(LENGTH HUXERUI_APP_RESOURCES HUXERUI_APP_RESOURCE_ROOT_COUNT)
    if (HUXERUI_APP_RESOURCE_ROOT_COUNT GREATER 1)
        message(FATAL_ERROR
                "huxerui_add_app() currently accepts one resource root"
        )
    endif ()
    set(HUXERUI_APP_INSTALL_COMPONENT "HuxerUIApplication")

    if (HUXERUI_LIBRARY_GRAPH_ONLY)
        if (NOT HUXERUI_LIBRARY_GRAPH_OUTPUT)
            message(FATAL_ERROR
                    "HUXERUI_LIBRARY_GRAPH_ONLY requires HUXERUI_LIBRARY_GRAPH_OUTPUT"
            )
        endif ()
        add_library(${target_name} INTERFACE)
        set_property(TARGET ${target_name} PROPERTY
                HUXERUI_LIBRARY_GRAPH_ONLY TRUE
        )
        set_property(TARGET ${target_name} PROPERTY
                HUXERUI_APPLICATION_INSTALL_COMPONENT
                "${HUXERUI_APP_INSTALL_COMPONENT}"
        )
        get_filename_component(HUXERUI_APP_LIBRARY_GRAPH_OUTPUT
                "${HUXERUI_LIBRARY_GRAPH_OUTPUT}"
                ABSOLUTE
                BASE_DIR "${CMAKE_BINARY_DIR}"
        )
        set_property(TARGET ${target_name} PROPERTY
                HUXERUI_LIBRARY_GRAPH_OUTPUT
                "${HUXERUI_APP_LIBRARY_GRAPH_OUTPUT}"
        )
        cmake_language(EVAL CODE
                "cmake_language(DEFER CALL _huxerui_write_library_graph [[${target_name}]])"
        )
        return()
    endif ()

    _huxerui_select_framework_target(HUXERUI_APP_FRAMEWORK_TARGET)
    get_target_property(HUXERUI_APP_FRAMEWORK_RESOURCE_PACKAGE
            ${HUXERUI_APP_FRAMEWORK_TARGET}
            HUXERUI_RESOURCE_PACKAGE
    )
    if (NOT HUXERUI_APP_FRAMEWORK_RESOURCE_PACKAGE
            OR HUXERUI_APP_FRAMEWORK_RESOURCE_PACKAGE MATCHES "-NOTFOUND$")
        message(FATAL_ERROR "huxerui_add_app() requires the HuxerUI built-in resource package")
    endif ()

    if (IOS)
        add_library(${target_name} STATIC ${HUXERUI_APP_SOURCES})
        set_target_properties(${target_name} PROPERTIES
                ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        )
    elseif (ANDROID)
        add_library(${target_name} SHARED ${HUXERUI_APP_SOURCES})
    else ()
        add_executable(${target_name} ${HUXERUI_APP_SOURCES})
    endif ()

    if (WIN32)
        set_target_properties(${target_name} PROPERTIES WIN32_EXECUTABLE TRUE)
        if (MSVC)
            # Preserve CRT initialization and the portable main() entry.
            set_property(TARGET ${target_name} PROPERTY HUXERUI_WINDOWS_CRT_ENTRY mainCRTStartup)
            target_link_options(${target_name} PRIVATE
                    "LINKER:/ENTRY:$<TARGET_PROPERTY:${target_name},HUXERUI_WINDOWS_CRT_ENTRY>"
            )
        endif ()
    endif ()

    set_property(TARGET ${target_name} PROPERTY
            HUXERUI_APPLICATION_INSTALL_COMPONENT
            "${HUXERUI_APP_INSTALL_COMPONENT}"
    )

    target_compile_features(${target_name} PRIVATE cxx_std_20)
    target_link_libraries(${target_name} PRIVATE
            ${HUXERUI_APP_FRAMEWORK_TARGET}
    )

    if (HUXERUI_APP_RESOURCE_OUTPUT_DIRECTORY)
        get_filename_component(HUXERUI_APP_RESOURCE_OUTPUT_DIRECTORY
                "${HUXERUI_APP_RESOURCE_OUTPUT_DIRECTORY}"
                ABSOLUTE
                BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}"
        )
        set_property(TARGET ${target_name} PROPERTY
                HUXERUI_RESOURCE_OUTPUT_DIRECTORY
                "${HUXERUI_APP_RESOURCE_OUTPUT_DIRECTORY}"
        )
    endif ()

    if (IOS)
        set_property(TARGET ${target_name} PROPERTY
                HUXERUI_RESOURCE_OUTPUT_DIRECTORY
                "${CMAKE_BINARY_DIR}/huxerui-ios/${target_name}/resources"
        )
    endif ()

    if (APPLE AND NOT IOS)
        set_target_properties(${target_name} PROPERTIES MACOSX_BUNDLE TRUE)
        if (HUXERUI_APP_BUNDLE_NAME)
            set_target_properties(${target_name} PROPERTIES
                    MACOSX_BUNDLE_BUNDLE_NAME "${HUXERUI_APP_BUNDLE_NAME}"
            )
        endif ()
        if (HUXERUI_APP_BUNDLE_IDENTIFIER)
            set_target_properties(${target_name} PROPERTIES
                    MACOSX_BUNDLE_GUI_IDENTIFIER "${HUXERUI_APP_BUNDLE_IDENTIFIER}"
            )
        endif ()
    endif ()

    huxerui_enable_codegen(${target_name})
    _huxerui_append_resource_package_input(
            ${target_name}
            "${HUXERUI_APP_FRAMEWORK_RESOURCE_PACKAGE}"
    )
    if (HUXERUI_APP_RESOURCES)
        list(GET HUXERUI_APP_RESOURCES 0 HUXERUI_APP_RESOURCE_ROOT)
        huxerui_add_resources(${target_name}
                ROOT "${HUXERUI_APP_RESOURCE_ROOT}"
                NAMESPACE "${HUXERUI_APP_RESOURCE_NAMESPACE}"
        )
    endif ()

    if (IOS)
        set_property(TARGET ${target_name} PROPERTY
                HUXERUI_APP_FRAMEWORK_TARGET
                "${HUXERUI_APP_FRAMEWORK_TARGET}"
        )
        _huxerui_schedule_ios_app_core(${target_name})
        return()
    endif ()

    _huxerui_json_escape("${target_name}" HUXERUI_APP_JSON_TARGET)
    _huxerui_json_escape("${HUXERUI_APP_BUNDLE_IDENTIFIER}" HUXERUI_APP_JSON_BUNDLE_IDENTIFIER)
    _huxerui_json_escape("${HUXERUI_APP_BUNDLE_NAME}" HUXERUI_APP_JSON_BUNDLE_NAME)

    set(HUXERUI_APP_INTEGRATION_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/huxerui-integration/${target_name}"
    )
    set(HUXERUI_APP_INTEGRATION_PLAN
            "${HUXERUI_APP_INTEGRATION_DIRECTORY}/$<CONFIG>/app.json"
    )
    if (ANDROID AND HUXERUI_ANDROID_APP_INTEGRATION_ROOT)
        # Gradle supplies a variant-specific root; each NDK configure owns exactly one ABI and configuration.
        set(HUXERUI_APP_INTEGRATION_DIRECTORY "${HUXERUI_ANDROID_APP_INTEGRATION_ROOT}/${ANDROID_ABI}")
        set(HUXERUI_APP_INTEGRATION_PLAN "${HUXERUI_APP_INTEGRATION_DIRECTORY}/app.json")
    endif ()
    set(HUXERUI_APP_BUNDLE_PATH)
    if (APPLE)
        set(HUXERUI_APP_BUNDLE_PATH "$<TARGET_BUNDLE_DIR:${target_name}>")
    endif ()
    file(MAKE_DIRECTORY "${HUXERUI_APP_INTEGRATION_DIRECTORY}")
    file(GENERATE
            OUTPUT "${HUXERUI_APP_INTEGRATION_PLAN}"
            CONTENT "{\n  \"schema\": 1,\n  \"target\": \"${HUXERUI_APP_JSON_TARGET}\",\n  \"name\": \"${HUXERUI_APP_JSON_BUNDLE_NAME}\",\n  \"version\": \"${PROJECT_VERSION}\",\n  \"artifact\": \"$<TARGET_FILE:${target_name}>\",\n  \"bundle\": \"${HUXERUI_APP_BUNDLE_PATH}\",\n  \"bundleIdentifier\": \"${HUXERUI_APP_JSON_BUNDLE_IDENTIFIER}\",\n  \"installComponent\": \"${HUXERUI_APP_INSTALL_COMPONENT}\"\n}\n"
    )
endfunction()
