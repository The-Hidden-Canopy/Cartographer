add_library(carto_d3d12 STATIC
    src/device.cpp
    src/frame_scheduler.cpp
    src/builtin_shader_cache.cpp
    src/projection_worker.cpp
    src/render_graph_dispatch.cpp
    src/swapchain.cpp
    src/texture_upload.cpp
)

target_include_directories(carto_d3d12
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

file(GLOB _carto_windows_sdk_um_dirs
    "$ENV{WindowsSdkDir}/Include/*/um"
    "C:/Program Files (x86)/Windows Kits/10/Include/*/um"
)
find_path(CARTO_DXC_INCLUDE_DIR
    NAMES dxcapi.h
    PATHS ${_carto_windows_sdk_um_dirs}
)
if(CARTO_DXC_INCLUDE_DIR)
    file(TO_CMAKE_PATH
        "${CARTO_DXC_INCLUDE_DIR}/dxcapi.h"
        CARTO_DXC_HEADER)
    target_compile_definitions(
        carto_d3d12 PRIVATE
            CARTO_D3D12_HAS_DXC=1
            CARTO_DXC_HEADER=\"${CARTO_DXC_HEADER}\")
else()
    target_compile_definitions(carto_d3d12 PRIVATE CARTO_D3D12_HAS_DXC=0)
endif()

target_link_libraries(carto_d3d12
    PUBLIC
        carto_core
        carto_gpu
        carto_device_ir
        carto_device
        carto_assets
        carto_render_graph
        d3d12
        dxgi
)

carto_target(carto_d3d12)

add_library(carto_d3d12_bake STATIC
    src/bake_graph.cpp
)
target_include_directories(carto_d3d12_bake
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        "${PROJECT_SOURCE_DIR}/libs/production/src"
)
target_link_libraries(carto_d3d12_bake
    PUBLIC
        carto_d3d12
        carto_production
)
carto_target(carto_d3d12_bake)

add_library(carto_d3d12_scene STATIC
    src/environment.cpp
    src/scene_renderer.cpp
    src/project_scene_renderer.cpp
    src/project_scene_textures.cpp
)
target_include_directories(carto_d3d12_scene PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(carto_d3d12_scene PUBLIC carto_d3d12 carto_production carto_render)
carto_target(carto_d3d12_scene)

install(TARGETS carto_d3d12 EXPORT CartographerTargets)
install(TARGETS carto_d3d12_bake EXPORT CartographerTargets)
install(TARGETS carto_d3d12_scene EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
install(DIRECTORY shaders/ DESTINATION share/cartographer/shaders
    FILES_MATCHING PATTERN "*.hlsl")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cartographer_authoring_motion.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cartographer_authoring_temporal.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cartographer_authoring_environment.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cartographer_authoring_surface.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cartographer_authoring_pbr.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cartographer_authoring_tonemap.hlsl")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cartographer_authoring_pbr.hlsl" CARTO_AUTHORING_PBR_SOURCE)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cartographer_authoring_environment.hlsl" CARTO_AUTHORING_ENVIRONMENT_SOURCE)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cartographer_authoring_surface.hlsl" CARTO_AUTHORING_SURFACE_SOURCE)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cartographer_authoring_motion.hlsl" CARTO_AUTHORING_MOTION_SOURCE)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cartographer_authoring_temporal.hlsl" CARTO_AUTHORING_TEMPORAL_SOURCE)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cartographer_authoring_tonemap.hlsl" CARTO_AUTHORING_DISPLAY_SOURCE)
configure_file(cmake/authoring_shader.hpp.in generated/authoring_shader.hpp @ONLY)
add_library(carto_d3d12_viewport STATIC src/authoring_viewport.cpp src/authoring_temporal.cpp)
target_include_directories(carto_d3d12_viewport PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(carto_d3d12_viewport PUBLIC carto_d3d12 carto_production)
carto_target(carto_d3d12_viewport)
install(TARGETS carto_d3d12_viewport EXPORT CartographerTargets)
