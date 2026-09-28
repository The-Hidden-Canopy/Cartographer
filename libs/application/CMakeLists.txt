add_library(carto_application STATIC
    src/application.cpp
)

target_include_directories(carto_application
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_application
    PUBLIC
        carto_core
        carto_geometry
        carto_journal
        carto_project
        carto_editor
        carto_render
)

carto_target(carto_application)

install(TARGETS carto_application EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
