add_library(carto_connection STATIC
    src/port.cpp
    src/graph.cpp
    src/attachment.cpp
)

target_include_directories(carto_connection
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_connection
    PUBLIC
        carto_core
        carto_assets
        carto_world
        carto_scene
)
carto_target(carto_connection)

install(TARGETS carto_connection EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
