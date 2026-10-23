add_library(carto_hydro STATIC
    src/bathymetry.cpp
    src/contact.cpp
    src/domain.cpp
    src/graph.cpp
    src/runoff.cpp
    src/terrain_routing.cpp
)

target_include_directories(carto_hydro
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_hydro
    PUBLIC
        carto_assets
        carto_core
)
carto_target(carto_hydro)

install(TARGETS carto_hydro EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
