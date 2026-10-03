add_executable(cartographer_geometry_bench
    geometry_kernels.cpp
)

target_link_libraries(cartographer_geometry_bench
    PRIVATE
        carto_geometry
)

carto_target(cartographer_geometry_bench)

add_executable(cartographer_web_geometry_bench
    web_geometry.cpp
)

target_link_libraries(cartographer_web_geometry_bench
    PRIVATE
        carto_web_geometry_compile
        carto_web_geometry_package
        carto_geometry
)

carto_target(cartographer_web_geometry_bench)

if(CARTO_ENABLE_D3D12)
    add_executable(cartographer_d3d12_kernel_bench
        d3d12_kernels.cpp
    )

    target_link_libraries(cartographer_d3d12_kernel_bench
        PRIVATE
            carto_d3d12
            carto_device
            carto_device_ir
            carto_gpu
    )
    target_compile_definitions(cartographer_d3d12_kernel_bench
        PRIVATE
            CARTO_D3D12_SHADER_DIR="${CMAKE_SOURCE_DIR}/libs/d3d12/shaders"
    )
    carto_target(cartographer_d3d12_kernel_bench)
endif()
