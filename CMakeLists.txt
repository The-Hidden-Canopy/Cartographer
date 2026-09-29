cmake_minimum_required(VERSION 3.25)

project(Cartographer
    VERSION 0.1.0
    DESCRIPTION "Standalone open-source spatial authoring system"
    LANGUAGES C CXX
)

option(CARTO_BUILD_TESTS "Build headless unit and invariant tests" ON)
option(CARTO_BUILD_CLI "Build the headless Cartographer CLI" ON)
option(CARTO_ENABLE_VULKAN "Enable the optional Vulkan render backend" OFF)
option(CARTO_BUILD_DESKTOP "Build the optional Dear ImGui/Vulkan desktop shell" OFF)
set(CARTO_IMGUI_ROOT "" CACHE PATH "Path to a pinned Dear ImGui source tree")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow)
elseif(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

function(carto_target target)
    target_compile_features(${target} PUBLIC cxx_std_23)
    target_compile_definitions(${target} PUBLIC CARTOGRAPHER_VERSION="${PROJECT_VERSION}")
endfunction()

add_subdirectory(libs/core)
add_subdirectory(libs/scene)
add_subdirectory(libs/geometry/mesh)
add_subdirectory(libs/assets)
add_subdirectory(libs/gpu)
add_subdirectory(libs/journal)
add_subdirectory(libs/render_graph)
add_subdirectory(libs/providers)
add_subdirectory(libs/plugin_protocol)
add_subdirectory(libs/project)
add_subdirectory(libs/editor)
add_subdirectory(libs/render)
add_subdirectory(libs/application)
add_subdirectory(libs/ui)
add_subdirectory(libs/sdk)
add_subdirectory(libs/io)

if(CARTO_ENABLE_VULKAN)
    find_package(Vulkan QUIET)
    if(NOT Vulkan_FOUND)
        message(FATAL_ERROR "CARTO_ENABLE_VULKAN=ON requires a discoverable Vulkan SDK")
    endif()
    add_subdirectory(libs/vulkan)
endif()

if(CARTO_BUILD_DESKTOP)
    if(NOT WIN32)
        message(FATAL_ERROR "CARTO_BUILD_DESKTOP=ON requires the Windows platform")
    endif()
    if(NOT CARTO_ENABLE_VULKAN)
        message(FATAL_ERROR "CARTO_BUILD_DESKTOP=ON requires CARTO_ENABLE_VULKAN=ON")
    endif()
    if(NOT EXISTS "${CARTO_IMGUI_ROOT}/imgui.h" OR
       NOT EXISTS "${CARTO_IMGUI_ROOT}/backends/imgui_impl_win32.cpp" OR
       NOT EXISTS "${CARTO_IMGUI_ROOT}/backends/imgui_impl_vulkan.cpp")
        message(FATAL_ERROR
            "CARTO_BUILD_DESKTOP=ON requires CARTO_IMGUI_ROOT pointing to a pinned Dear ImGui source tree")
    endif()
    add_subdirectory(apps/desktop)
endif()

if(CARTO_BUILD_CLI)
    add_subdirectory(apps/cli)
endif()

if(CARTO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(EXPORT CartographerTargets
    FILE CartographerTargets.cmake
    NAMESPACE Cartographer::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Cartographer
)

configure_package_config_file(
    cmake/CartographerConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/CartographerConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Cartographer
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/CartographerConfigVersion.cmake
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/CartographerConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/CartographerConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Cartographer
)

install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DOCDIR})
