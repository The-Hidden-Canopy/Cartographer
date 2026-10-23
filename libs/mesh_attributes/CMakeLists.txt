add_library(carto_mesh_attributes STATIC
    src/provenance.cpp
)

target_include_directories(carto_mesh_attributes
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_mesh_attributes PUBLIC carto_attributes carto_geometry)
carto_target(carto_mesh_attributes)

install(TARGETS carto_mesh_attributes EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
