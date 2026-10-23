add_library(carto_navgraph STATIC
    src/access.cpp
    src/agent_profile.cpp
    src/envelope.cpp
    src/invalidation.cpp
    src/links.cpp
    src/port_reference.cpp
    src/region.cpp
    src/route.cpp
)

target_include_directories(carto_navgraph
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_navgraph
    PUBLIC
        carto_core
        carto_world
        carto_scene
)
carto_target(carto_navgraph)

install(TARGETS carto_navgraph EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
