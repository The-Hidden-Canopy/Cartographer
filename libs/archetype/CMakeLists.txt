add_library(carto_archetype STATIC
    src/digest.cpp
    src/identity.cpp
    src/inference.cpp
)

target_include_directories(carto_archetype
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_archetype PUBLIC carto_world carto_core carto_scene carto_geometry)
carto_target(carto_archetype)

install(TARGETS carto_archetype EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
