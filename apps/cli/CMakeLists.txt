add_executable(cartographer_cli
    main.cpp
)

target_link_libraries(cartographer_cli
    PRIVATE
        carto_core
        carto_scene
        carto_geometry
        carto_project
        carto_editor
        carto_render
        carto_application
        carto_io
        carto_production
        carto_providers
)

carto_target(cartographer_cli)

install(TARGETS cartographer_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
