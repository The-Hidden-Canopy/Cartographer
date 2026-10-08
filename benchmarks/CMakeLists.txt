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
