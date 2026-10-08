add_library(carto_production STATIC
    src/artifact_store.cpp
    src/closure.cpp
    src/decision.cpp
    src/dependency.cpp
    src/lookdev_draw_package.cpp
    src/material_artifact.cpp
    src/render_mesh.cpp
)

target_include_directories(carto_production
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_production
    PUBLIC
        carto_core
        carto_assets
        carto_project
        carto_geometry
        carto_render
)
carto_target(carto_production)

install(TARGETS carto_production EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
