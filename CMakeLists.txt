cmake_minimum_required(VERSION 3.25)

project(Cartographer
    VERSION 0.1.0
    DESCRIPTION "Standalone spatial authoring system"
    LANGUAGES C CXX
)

option(CARTO_BUILD_TESTS "Build headless unit and invariant tests" ON)
option(CARTO_BUILD_BENCHMARKS "Build native CPU geometry and scientific kernel benchmarks" OFF)
option(CARTO_BUILD_CLI "Build the headless Cartographer CLI" ON)
option(CARTO_BUILD_DEVICE_CPU "Build the CPU reference device" ON)
option(CARTO_BUILD_SDK_SHARED "Build the public C ABI as an opt-in shared library" OFF)
option(CARTO_ENABLE_VULKAN "Enable the optional Vulkan render backend" OFF)
set(CARTO_VULKAN_SDK_ROOT "" CACHE PATH
    "Vulkan SDK root used for headers, libraries, and shader tools")
set(CARTO_GLSLC_EXECUTABLE "" CACHE FILEPATH
    "Path to glslc used to compile Cartographer Vulkan shaders")
option(CARTO_ENABLE_D3D12 "Enable the optional Windows D3D12 render backend" OFF)
option(CARTO_BUILD_D3D12_DESKTOP
    "Build the optional native Win32/D3D12 presentation shell" OFF)
option(CARTO_BUILD_DESKTOP "Build the optional Dear ImGui/Vulkan desktop shell" OFF)
set(CARTO_IMGUI_ROOT "" CACHE PATH "Path to a pinned Dear ImGui source tree")
option(CARTO_USE_BUNDLED_IMGUI
    "Use the pinned public Dear ImGui source included in third_party/imgui" ON)
if(CARTO_USE_BUNDLED_IMGUI AND CARTO_IMGUI_ROOT STREQUAL "" AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui.h")
    set(CARTO_IMGUI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui" CACHE PATH
        "Path to a pinned Dear ImGui source tree" FORCE)
endif()

# Cartographer can be built standalone or added as a public subdirectory of a
# consumer such as CONDITION. Resolve the public boundary from this checkout,
# not from whichever project owns the outer CMake configure.
get_filename_component(_carto_public_root "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
set(_carto_protected_public_paths
    "${_carto_public_root}/libs/web_geometry/certified"
    "${_carto_public_root}/libs/polygon"
    "${_carto_public_root}/tests/certified_geometry_tests.cpp"
    "${_carto_public_root}/tests/polygon_tests.cpp"
    "${_carto_public_root}/benchmarks/certified_geometry.cpp"
    "${_carto_public_root}/benchmarks/polygon.cpp"
    "${_carto_public_root}/docs/architecture/consequence-certified-adaptive-geometry.md"
    "${_carto_public_root}/docs/verification/consequence-certified-adaptive-geometry.md"
    "${_carto_public_root}/docs/architecture/persistent-polygons.md"
    "${_carto_public_root}/docs/verification/persistent-polygons.md"
)
foreach(_carto_protected_path IN LISTS _carto_protected_public_paths)
    if(EXISTS "${_carto_protected_path}")
        message(FATAL_ERROR
            "protected Cartographer algorithm surface must remain outside the public checkout: ${_carto_protected_path}")
    endif()
endforeach()

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
add_subdirectory(libs/attributes)
add_subdirectory(libs/geometry/mesh)
add_subdirectory(libs/botanical_contract)
add_subdirectory(libs/assets)
add_subdirectory(libs/runtime_api)
add_subdirectory(libs/hydro)
add_subdirectory(libs/spatial_field)
add_subdirectory(libs/terrain)
add_subdirectory(libs/temporal)
add_subdirectory(libs/surface/core)
add_subdirectory(libs/physical)
add_subdirectory(libs/scientific)
add_subdirectory(libs/world)
add_subdirectory(libs/semantic)
add_subdirectory(libs/connection)
add_subdirectory(libs/navgraph)
add_subdirectory(libs/kinematic)
add_subdirectory(libs/character)
add_subdirectory(libs/weights)
add_subdirectory(libs/pose)
add_subdirectory(libs/deform)
add_subdirectory(libs/face)
add_subdirectory(libs/motion)
add_subdirectory(libs/sim)
add_subdirectory(libs/gpudef)
add_subdirectory(libs/archetype)
add_subdirectory(libs/mesh_attributes)
add_subdirectory(libs/web_geometry/core)
add_subdirectory(libs/collision)
add_subdirectory(libs/biological)
add_subdirectory(libs/production_authoring)
add_subdirectory(libs/condition_authoring)
add_subdirectory(libs/engineering)
add_subdirectory(libs/web_geometry/compile)
add_subdirectory(libs/web_geometry/package)
add_subdirectory(libs/eval)
add_subdirectory(libs/web_geometry/evaluation)
add_subdirectory(libs/gpu)
add_subdirectory(libs/journal)
add_subdirectory(libs/render_graph)
add_subdirectory(libs/device_ir)
add_subdirectory(libs/device)
if(CARTO_BUILD_DEVICE_CPU)
    add_subdirectory(libs/device_cpu)
endif()
add_subdirectory(libs/providers)
add_subdirectory(libs/plugin_protocol)
add_subdirectory(libs/plugin_package)
add_subdirectory(libs/market)
add_subdirectory(libs/project)
add_subdirectory(libs/workspace)
add_subdirectory(libs/editor)
add_subdirectory(libs/ai)
add_subdirectory(libs/render)
add_subdirectory(libs/production)
add_subdirectory(libs/verification)
add_subdirectory(libs/application)
add_subdirectory(libs/agent)
add_subdirectory(libs/ui)
add_subdirectory(libs/sdk)
add_subdirectory(libs/io)

if(CARTO_ENABLE_VULKAN)
    if(CARTO_VULKAN_SDK_ROOT STREQUAL "" AND DEFINED ENV{VULKAN_SDK})
        set(CARTO_VULKAN_SDK_ROOT "$ENV{VULKAN_SDK}" CACHE PATH
            "Vulkan SDK root used for headers, libraries, and shader tools" FORCE)
    endif()
    if(CARTO_VULKAN_SDK_ROOT STREQUAL "" AND WIN32)
        file(GLOB _carto_vulkan_sdk_candidates LIST_DIRECTORIES true "C:/VulkanSDK/*")
        list(SORT _carto_vulkan_sdk_candidates COMPARE NATURAL ORDER DESCENDING)
        foreach(_carto_vulkan_sdk_candidate IN LISTS _carto_vulkan_sdk_candidates)
            if(EXISTS "${_carto_vulkan_sdk_candidate}/Include/vulkan/vulkan.h" AND
               EXISTS "${_carto_vulkan_sdk_candidate}/Lib/vulkan-1.lib")
                set(CARTO_VULKAN_SDK_ROOT "${_carto_vulkan_sdk_candidate}" CACHE PATH
                    "Vulkan SDK root used for headers, libraries, and shader tools" FORCE)
                break()
            endif()
        endforeach()
    endif()
    if(NOT CARTO_VULKAN_SDK_ROOT STREQUAL "")
        list(PREPEND CMAKE_PREFIX_PATH "${CARTO_VULKAN_SDK_ROOT}")
        set(Vulkan_ROOT "${CARTO_VULKAN_SDK_ROOT}")
    endif()
    find_package(Vulkan QUIET)
    if(NOT Vulkan_FOUND AND NOT CARTO_VULKAN_SDK_ROOT STREQUAL "")
        find_path(CARTO_VULKAN_INCLUDE_DIR
            NAMES vulkan/vulkan.h
            HINTS "${CARTO_VULKAN_SDK_ROOT}/Include"
            NO_DEFAULT_PATH
        )
        find_library(CARTO_VULKAN_LIBRARY
            NAMES vulkan-1
            HINTS "${CARTO_VULKAN_SDK_ROOT}/Lib"
            NO_DEFAULT_PATH
        )
        if(CARTO_VULKAN_INCLUDE_DIR AND CARTO_VULKAN_LIBRARY)
            if(NOT TARGET Vulkan::Vulkan)
                add_library(Vulkan::Vulkan UNKNOWN IMPORTED)
            endif()
            set_target_properties(Vulkan::Vulkan PROPERTIES
                IMPORTED_LOCATION "${CARTO_VULKAN_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${CARTO_VULKAN_INCLUDE_DIR}"
            )
            set(Vulkan_FOUND TRUE)
        endif()
    endif()
    if(NOT Vulkan_FOUND)
        message(FATAL_ERROR "CARTO_ENABLE_VULKAN=ON requires a discoverable Vulkan SDK")
    endif()
    add_subdirectory(libs/vulkan)
endif()

if(CARTO_ENABLE_D3D12)
    if(NOT WIN32)
        message(FATAL_ERROR "CARTO_ENABLE_D3D12=ON requires the Windows platform")
    endif()
    add_subdirectory(libs/d3d12)
endif()

if(CARTO_BUILD_D3D12_DESKTOP)
    if(NOT WIN32)
        message(FATAL_ERROR "CARTO_BUILD_D3D12_DESKTOP=ON requires the Windows platform")
    endif()
    if(NOT CARTO_ENABLE_D3D12)
        message(FATAL_ERROR
            "CARTO_BUILD_D3D12_DESKTOP=ON requires CARTO_ENABLE_D3D12=ON")
    endif()
    add_subdirectory(apps/d3d12_desktop)
endif()

if(CARTO_BUILD_DESKTOP)
    if(NOT WIN32)
        message(FATAL_ERROR "CARTO_BUILD_DESKTOP=ON requires the Windows platform")
    endif()
    if(NOT CARTO_ENABLE_VULKAN)
        message(FATAL_ERROR "CARTO_BUILD_DESKTOP=ON requires CARTO_ENABLE_VULKAN=ON")
    endif()
    set(_carto_imgui_required_files
        imgui.h
        imgui.cpp
        imgui_draw.cpp
        imgui_tables.cpp
        imgui_widgets.cpp
        backends/imgui_impl_win32.h
        backends/imgui_impl_win32.cpp
        backends/imgui_impl_vulkan.h
        backends/imgui_impl_vulkan.cpp
    )
    set(_carto_imgui_missing_files)
    foreach(_carto_imgui_required_file IN LISTS _carto_imgui_required_files)
        if(NOT EXISTS "${CARTO_IMGUI_ROOT}/${_carto_imgui_required_file}")
            list(APPEND _carto_imgui_missing_files "${_carto_imgui_required_file}")
        endif()
    endforeach()
    if(_carto_imgui_missing_files)
        message(FATAL_ERROR
            "CARTO_BUILD_DESKTOP=ON requires CARTO_IMGUI_ROOT with the pinned Dear ImGui sources; missing: ${_carto_imgui_missing_files}")
    endif()
    file(STRINGS "${CARTO_IMGUI_ROOT}/imgui.h" _carto_imgui_docking_marker
        REGEX "^#define IMGUI_HAS_DOCK")
    if(NOT _carto_imgui_docking_marker)
        message(FATAL_ERROR
            "CARTO_BUILD_DESKTOP=ON requires the pinned Dear ImGui docking branch (see third_party/imgui/CARTOGRAPHER_PIN)")
    endif()
    add_subdirectory(apps/desktop)
endif()

if(CARTO_BUILD_CLI)
    add_subdirectory(apps/cli)
endif()

if(CARTO_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
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
    cmake/CartographerSQLite3.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Cartographer
)

install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DOCDIR})
