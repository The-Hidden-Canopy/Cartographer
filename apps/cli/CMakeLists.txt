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
        carto_ui
        carto_io
        carto_production
        carto_providers
)

carto_target(cartographer_cli)

if(TARGET carto_d3d12_bake)
    target_link_libraries(cartographer_cli PRIVATE carto_d3d12_bake)
    target_compile_definitions(cartographer_cli PRIVATE CARTO_CLI_HAS_D3D12_BAKE=1)
else()
    target_compile_definitions(cartographer_cli PRIVATE CARTO_CLI_HAS_D3D12_BAKE=0)
endif()

install(TARGETS cartographer_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(TARGET carto_d3d12_scene)
    add_executable(cartographer_scene_render scene_render.cpp)
    target_link_libraries(cartographer_scene_render PRIVATE carto_d3d12_scene)
    carto_target(cartographer_scene_render)
    install(TARGETS cartographer_scene_render RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
