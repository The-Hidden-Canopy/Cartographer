add_library(carto_collision STATIC
    src/benchmark.cpp
    src/compound.cpp
    src/convex.cpp
    src/family.cpp
    src/metrics.cpp
    src/policy.cpp
    src/primitive.cpp
    src/profile.cpp
    src/triangle.cpp
)

target_include_directories(carto_collision
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_collision
    PUBLIC
        carto_core
        carto_assets
        carto_geometry
        carto_web_geometry_core
)
carto_target(carto_collision)

install(TARGETS carto_collision EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
