add_library(carto_render STATIC
    src/render_scene.cpp
)

target_include_directories(carto_render
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_render PUBLIC carto_core carto_scene carto_geometry)
carto_target(carto_render)

install(TARGETS carto_render EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)

