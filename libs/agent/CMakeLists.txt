add_library(carto_agent STATIC
    src/workbench.cpp
)

target_include_directories(carto_agent
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_agent
    PUBLIC
        carto_ai
        carto_application
        carto_assets
        carto_core
        carto_geometry
        carto_scene
)

carto_target(carto_agent)

install(TARGETS carto_agent EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
