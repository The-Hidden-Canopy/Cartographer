add_library(carto_editor STATIC
    src/authoring_context.cpp
    src/command_bus.cpp
    src/preview_transaction.cpp
    src/selection.cpp
    src/tools.cpp
)

target_include_directories(carto_editor
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_editor PUBLIC carto_core carto_scene carto_geometry carto_project)
carto_target(carto_editor)

install(TARGETS carto_editor EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
