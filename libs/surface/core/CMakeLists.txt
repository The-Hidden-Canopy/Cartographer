add_library(carto_surface_core STATIC
    src/core.cpp
    src/process.cpp
)

target_include_directories(carto_surface_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_surface_core
    PUBLIC
        carto_assets
        carto_geometry
)

carto_target(carto_surface_core)

install(TARGETS carto_surface_core EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
