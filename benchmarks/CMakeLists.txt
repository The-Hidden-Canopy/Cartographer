add_executable(cartographer_geometry_bench
    geometry_kernels.cpp
)

target_link_libraries(cartographer_geometry_bench
    PRIVATE
        carto_geometry
)

carto_target(cartographer_geometry_bench)
