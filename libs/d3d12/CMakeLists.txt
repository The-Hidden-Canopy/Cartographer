add_library(carto_d3d12 STATIC
    src/device.cpp
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
        d3d12
        dxgi
)

carto_target(carto_d3d12)

install(TARGETS carto_d3d12 EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
install(DIRECTORY shaders/ DESTINATION share/cartographer/shaders
    FILES_MATCHING PATTERN "*.hlsl")
