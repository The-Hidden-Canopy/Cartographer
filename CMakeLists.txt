cmake_minimum_required(VERSION 3.25)

project(Cartographer
    VERSION 0.1.0
    DESCRIPTION "Standalone spatial authoring system"
    LANGUAGES C CXX
)

option(CARTO_BUILD_TESTS "Build headless unit and invariant tests" ON)
option(CARTO_BUILD_BENCHMARKS "Build native geometry kernel benchmarks" OFF)
option(CARTO_BUILD_CLI "Build the headless Cartographer CLI" ON)
option(CARTO_BUILD_DEVICE_CPU "Build the CPU reference device" ON)
option(CARTO_BUILD_SDK_SHARED "Build the public C ABI as an opt-in shared library" OFF)
option(CARTO_BUILD_PRIVATE_INTEGRATIONS
    "Build out-of-tree moat or not-yet-cleared integration surfaces" OFF)
set(CARTO_PRIVATE_SOURCE_ROOT "" CACHE PATH
    "Out-of-tree Cartographer-private source root")
option(CARTO_ENABLE_VULKAN "Enable the optional Vulkan render backend" OFF)
option(CARTO_ENABLE_D3D12 "Enable the optional Windows D3D12 render backend" OFF)
option(CARTO_BUILD_D3D12_DESKTOP
    "Build the optional native Win32/D3D12 presentation shell" OFF)
option(CARTO_BUILD_DESKTOP "Build the optional Dear ImGui/Vulkan desktop shell" OFF)
set(CARTO_IMGUI_ROOT "" CACHE PATH "Path to a pinned Dear ImGui source tree")
option(CARTO_BUILD_GMIB_GRAPHICS "Build the optional GMIB presentation consumer" OFF)
set(CARTO_GMIB_ROOT "" CACHE PATH "Standalone GMIB source root for the optional graphics consumer")
option(CARTO_BUILD_GMIB_GAME "Build the Cartographer-hosted GMIB game surface" OFF)

get_filename_component(_carto_public_root "${CMAKE_SOURCE_DIR}" REALPATH)
if(CARTO_BUILD_PRIVATE_INTEGRATIONS)
    if(CARTO_PRIVATE_SOURCE_ROOT STREQUAL "" OR
       NOT IS_DIRECTORY "${CARTO_PRIVATE_SOURCE_ROOT}")
        message(FATAL_ERROR
            "CARTO_BUILD_PRIVATE_INTEGRATIONS=ON requires an existing out-of-tree CARTO_PRIVATE_SOURCE_ROOT")
    endif()
    get_filename_component(_carto_private_root "${CARTO_PRIVATE_SOURCE_ROOT}" REALPATH)
    cmake_path(IS_PREFIX _carto_public_root "${_carto_private_root}" NORMALIZE _carto_private_inside_public)
    if(_carto_private_inside_public)
        message(FATAL_ERROR
            "CARTO_PRIVATE_SOURCE_ROOT must be outside the public Cartographer checkout")
    endif()
    set(_carto_private_required_paths
        "${CARTO_PRIVATE_SOURCE_ROOT}/libs/anatomy/CMakeLists.txt"
        "${CARTO_PRIVATE_SOURCE_ROOT}/libs/owh/CMakeLists.txt"
        "${CARTO_PRIVATE_SOURCE_ROOT}/libs/simulation/CMakeLists.txt"
        "${CARTO_PRIVATE_SOURCE_ROOT}/tests/anatomy_catalog_tests.cpp")
    foreach(_carto_private_path IN LISTS _carto_private_required_paths)
        if(NOT EXISTS "${_carto_private_path}")
            message(FATAL_ERROR
                "CARTO_BUILD_PRIVATE_INTEGRATIONS=ON requires a complete CARTO_PRIVATE_SOURCE_ROOT: missing ${_carto_private_path}")
        endif()
    endforeach()
endif()
if((CARTO_BUILD_GMIB_GRAPHICS OR CARTO_BUILD_GMIB_GAME) AND
   NOT CARTO_BUILD_PRIVATE_INTEGRATIONS)
    message(FATAL_ERROR
        "GMIB surfaces are out-of-tree integrations; enable CARTO_BUILD_PRIVATE_INTEGRATIONS and provide CARTO_PRIVATE_SOURCE_ROOT")
endif()
if(CARTO_BUILD_GMIB_GRAPHICS AND
   NOT EXISTS "${CARTO_PRIVATE_SOURCE_ROOT}/libs/gmib/CMakeLists.txt")
    message(FATAL_ERROR
        "CARTO_BUILD_GMIB_GRAPHICS=ON requires the out-of-tree GMIB adapter in CARTO_PRIVATE_SOURCE_ROOT")
endif()
if(CARTO_BUILD_GMIB_GRAPHICS AND
   NOT EXISTS "${CARTO_GMIB_ROOT}/include/gmib/game/presentation.hpp")
    message(FATAL_ERROR "CARTO_BUILD_GMIB_GRAPHICS=ON requires CARTO_GMIB_ROOT pointing at god-mode-is-a-bug")
endif()
if(CARTO_BUILD_GMIB_GRAPHICS)
    get_filename_component(_carto_gmib_root "${CARTO_GMIB_ROOT}" REALPATH)
    cmake_path(IS_PREFIX _carto_public_root "${_carto_gmib_root}" NORMALIZE _carto_gmib_inside_public)
    if(_carto_gmib_inside_public)
        message(FATAL_ERROR
            "CARTO_GMIB_ROOT must be outside the public Cartographer checkout")
    endif()
endif()
if(CARTO_BUILD_GMIB_GAME AND NOT CARTO_BUILD_GMIB_GRAPHICS)
    message(FATAL_ERROR "CARTO_BUILD_GMIB_GAME=ON requires CARTO_BUILD_GMIB_GRAPHICS=ON")
endif()
if(CARTO_BUILD_GMIB_GAME AND
   (NOT EXISTS "${CARTO_PRIVATE_SOURCE_ROOT}/apps/gmib-game/main.cpp" OR
    NOT EXISTS "${CARTO_PRIVATE_SOURCE_ROOT}/apps/gmib-game/bake.cpp"))
    message(FATAL_ERROR
        "CARTO_BUILD_GMIB_GAME=ON requires the out-of-tree GMIB host sources in CARTO_PRIVATE_SOURCE_ROOT")
endif()
if(CARTO_BUILD_GMIB_GAME AND
   NOT EXISTS "${CARTO_GMIB_ROOT}/include/gmib/game/game.hpp")
    message(FATAL_ERROR "CARTO_BUILD_GMIB_GAME=ON requires CARTO_GMIB_ROOT pointing at god-mode-is-a-bug")
endif()

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
if(CARTO_BUILD_PRIVATE_INTEGRATIONS)
    add_subdirectory(
        "${CARTO_PRIVATE_SOURCE_ROOT}/libs/anatomy"
        "${CMAKE_CURRENT_BINARY_DIR}/private/anatomy")
    add_subdirectory(
        "${CARTO_PRIVATE_SOURCE_ROOT}/libs/owh"
        "${CMAKE_CURRENT_BINARY_DIR}/private/owh")
    add_subdirectory(
        "${CARTO_PRIVATE_SOURCE_ROOT}/libs/simulation"
        "${CMAKE_CURRENT_BINARY_DIR}/private/simulation")
endif()
add_subdirectory(libs/geometry/mesh)
add_subdirectory(libs/assets)
add_subdirectory(libs/attributes)
add_subdirectory(libs/mesh_attributes)
add_subdirectory(libs/web_geometry/core)
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
add_subdirectory(libs/project)
add_subdirectory(libs/editor)
add_subdirectory(libs/ai)
add_subdirectory(libs/render)
if(CARTO_BUILD_GMIB_GRAPHICS)
    add_subdirectory(
        "${CARTO_PRIVATE_SOURCE_ROOT}/libs/gmib"
        "${CMAKE_CURRENT_BINARY_DIR}/private/gmib")
endif()

if(CARTO_BUILD_GMIB_GAME)
    # Build GMIB from source inside the Cartographer host configuration. The
    # standalone checkout remains independently buildable, while this target
    # no longer relies on a separately configured GMIB binary directory.
    set(_carto_gmib_tests_was_defined FALSE)
    if(DEFINED GMIB_BUILD_TESTS)
        set(_carto_gmib_tests_was_defined TRUE)
        set(_carto_gmib_tests_previous "${GMIB_BUILD_TESTS}")
    endif()
    set(GMIB_BUILD_TESTS OFF CACHE BOOL "Build GMIB tests when embedded by an engine" FORCE)
    add_subdirectory("${CARTO_GMIB_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/gmib" EXCLUDE_FROM_ALL)
    if(_carto_gmib_tests_was_defined)
        set(GMIB_BUILD_TESTS "${_carto_gmib_tests_previous}" CACHE BOOL
            "Build GMIB tests when embedded by an engine" FORCE)
    else()
        unset(GMIB_BUILD_TESTS CACHE)
    endif()
    if(NOT TARGET gmib_game)
        message(FATAL_ERROR "embedded GMIB source did not define the gmib_game target")
    endif()

    add_executable(
        cartographer_gmib_game
        "${CARTO_PRIVATE_SOURCE_ROOT}/apps/gmib-game/main.cpp")
    target_link_libraries(cartographer_gmib_game PRIVATE carto_gmib_graphics gmib_game)
    carto_target(cartographer_gmib_game)
endif()
add_subdirectory(libs/application)
add_subdirectory(libs/ui)
add_subdirectory(libs/sdk)
add_subdirectory(libs/io)

if(CARTO_BUILD_GMIB_GAME)
    target_link_libraries(cartographer_gmib_game PRIVATE carto_project)
    add_executable(
        cartographer_gmib_bake
        "${CARTO_PRIVATE_SOURCE_ROOT}/apps/gmib-game/bake.cpp")
    target_link_libraries(cartographer_gmib_bake PRIVATE carto_project carto_assets carto_gmib_graphics gmib_game)
    carto_target(cartographer_gmib_bake)
endif()

if(CARTO_ENABLE_VULKAN)
    find_package(Vulkan QUIET)
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

if(CARTO_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(CARTO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)

    if(CARTO_BUILD_PRIVATE_INTEGRATIONS)
        add_executable(cartographer_simulation_tests
            "${CARTO_PRIVATE_SOURCE_ROOT}/libs/simulation/tests/simulation_tests.cpp")
        target_link_libraries(cartographer_simulation_tests PRIVATE carto_simulation)
        carto_target(cartographer_simulation_tests)
        add_test(NAME cartographer_simulation_tests COMMAND cartographer_simulation_tests)
    endif()

    if(CARTO_BUILD_GMIB_GRAPHICS)
        add_executable(cartographer_gmib_graphics_tests
            "${CARTO_PRIVATE_SOURCE_ROOT}/libs/gmib/tests/graphics_tests.cpp"
        )
        target_link_libraries(cartographer_gmib_graphics_tests PRIVATE carto_gmib_graphics)
        carto_target(cartographer_gmib_graphics_tests)
        add_test(NAME cartographer_gmib_graphics_tests COMMAND cartographer_gmib_graphics_tests)
    endif()

    if(CARTO_BUILD_GMIB_GAME)
        add_executable(cartographer_gmib_game_tests
            "${CARTO_PRIVATE_SOURCE_ROOT}/libs/gmib/tests/game_host_tests.cpp"
        )
        target_link_libraries(cartographer_gmib_game_tests PRIVATE carto_gmib_graphics gmib_game)
        carto_target(cartographer_gmib_game_tests)
        add_test(NAME cartographer_gmib_game_tests COMMAND cartographer_gmib_game_tests)
    endif()

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
