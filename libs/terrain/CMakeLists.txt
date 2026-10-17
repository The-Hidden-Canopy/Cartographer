add_library(carto_terrain STATIC
    src/terrain_tile.cpp
)

target_include_directories(carto_terrain
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_terrain PUBLIC carto_hydro carto_spatial_field)
carto_target(carto_terrain)

install(TARGETS carto_terrain EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
