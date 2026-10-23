add_library(carto_scene STATIC
    src/scene.cpp
)

target_include_directories(carto_scene
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_scene PUBLIC carto_core)
carto_target(carto_scene)

install(TARGETS carto_scene EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)

