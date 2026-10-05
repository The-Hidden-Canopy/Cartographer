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

add_executable(cartographer_surface_closure_bench
    surface_closure.cpp
)

target_link_libraries(cartographer_surface_closure_bench
    PRIVATE
        carto_surface_core
        carto_geometry
)

carto_target(cartographer_surface_closure_bench)

add_executable(cartographer_surface_workflow_bench
    surface_workflow.cpp
)
target_link_libraries(cartographer_surface_workflow_bench
    PRIVATE
        carto_surface_core
)
carto_target(cartographer_surface_workflow_bench)

add_executable(cartographer_surface_spectrum_bench
    surface_spectrum.cpp
)
target_link_libraries(cartographer_surface_spectrum_bench
    PRIVATE
        carto_surface_core
        carto_geometry
)
carto_target(cartographer_surface_spectrum_bench)

add_executable(cartographer_surface_distribution_bench
    surface_distribution.cpp
)
target_link_libraries(cartographer_surface_distribution_bench
    PRIVATE
        carto_surface_core
)
carto_target(cartographer_surface_distribution_bench)

add_executable(cartographer_surface_qem_bench
    surface_qem.cpp
)
target_link_libraries(cartographer_surface_qem_bench
    PRIVATE
        carto_surface_core
        carto_geometry
)
carto_target(cartographer_surface_qem_bench)

add_executable(cartographer_surface_normal_closure_bench
    surface_normal_closure.cpp
)
target_link_libraries(cartographer_surface_normal_closure_bench
    PRIVATE
        carto_surface_core
        carto_geometry
)
carto_target(cartographer_surface_normal_closure_bench)

add_executable(cartographer_surface_visibility_closure_bench
    surface_visibility_closure.cpp
)
target_link_libraries(cartographer_surface_visibility_closure_bench
    PRIVATE
        carto_surface_core
        carto_geometry
)
carto_target(cartographer_surface_visibility_closure_bench)

add_executable(cartographer_scientific_bench
    scientific_kernels.cpp
)

target_link_libraries(cartographer_scientific_bench
    PRIVATE
        carto_device
        carto_scientific
)

carto_target(cartographer_scientific_bench)

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
            CARTO_D3D12_SHADER_DIR="${CMAKE_CURRENT_LIST_DIR}/../libs/d3d12/shaders"
    )
    carto_target(cartographer_d3d12_kernel_bench)
endif()
