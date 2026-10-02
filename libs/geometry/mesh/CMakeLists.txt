add_library(carto_geometry STATIC
    src/mesh.cpp
    src/primitives.cpp
    src/topology_edit.cpp
)

target_include_directories(carto_geometry
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_geometry PUBLIC carto_core)
carto_target(carto_geometry)

install(TARGETS carto_geometry EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
