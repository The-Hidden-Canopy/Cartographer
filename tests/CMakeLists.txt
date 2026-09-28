add_executable(cartographer_tests
    test_main.cpp
)

target_link_libraries(cartographer_tests
    PRIVATE
        carto_core
        carto_scene
        carto_geometry
        carto_project
        carto_editor
        carto_render
        carto_io
)

carto_target(cartographer_tests)
add_test(NAME cartographer_tests COMMAND cartographer_tests)

