cmake_minimum_required(VERSION 3.20)
project(HuxerUIWindowsPaintBenchmark LANGUAGES CXX)

if (NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "This benchmark requires the existing Windows MSVC framework build")
endif ()
get_filename_component(HUXERUI_PROJECT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(HUXERUI_FRAMEWORK_BUILD "" CACHE PATH "Compatible framework build containing the Release static library")
set(HUXERUI_BASELINE_RENDERER "" CACHE FILEPATH "Unmodified renderer source extracted from the baseline commit")
set(HUXERUI_OPTIMIZED_RENDERER "${HUXERUI_PROJECT_DIR}/platform/windows/win32_renderer.cpp" CACHE FILEPATH
        "Working-tree renderer snapshot")
if (NOT EXISTS "${HUXERUI_FRAMEWORK_BUILD}/lib/Release/huxerui_static.lib" OR
    NOT EXISTS "${HUXERUI_BASELINE_RENDERER}")
    message(FATAL_ERROR "Build the Release framework and supply its build directory and the baseline renderer")
endif ()

# Consume the platform's authoritative native link interface without changing the framework build.
include("${HUXERUI_PROJECT_DIR}/cmake/platform/Windows.cmake")
foreach (variant IN ITEMS baseline optimized)
    add_executable(paint_benchmark_${variant} benchmark.cpp)
    set_target_properties(paint_benchmark_${variant} PROPERTIES CXX_EXTENSIONS OFF)
    target_compile_features(paint_benchmark_${variant} PRIVATE cxx_std_20)
    target_compile_options(paint_benchmark_${variant} PRIVATE /W4 /permissive- /utf-8)
    target_include_directories(paint_benchmark_${variant} PRIVATE
            "${HUXERUI_PROJECT_DIR}/include" "${HUXERUI_PROJECT_DIR}/src"
            "${HUXERUI_PROJECT_DIR}/platform/windows")
    target_compile_definitions(paint_benchmark_${variant} PRIVATE ${HUXERUI_PLATFORM_COMPILE_DEFINITIONS})
    target_link_libraries(paint_benchmark_${variant} PRIVATE
            "${HUXERUI_FRAMEWORK_BUILD}/lib/Release/huxerui_static.lib" ${HUXERUI_PLATFORM_LINK_LIBRARIES})
    if (variant STREQUAL "baseline")
        set(renderer "${HUXERUI_BASELINE_RENDERER}")
    else ()
        set(renderer "${HUXERUI_OPTIMIZED_RENDERER}")
    endif ()
    target_compile_definitions(paint_benchmark_${variant} PRIVATE "HUXERUI_BENCHMARK_RENDERER=\"${renderer}\"")
endforeach ()
