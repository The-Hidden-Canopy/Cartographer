add_library(carto_application STATIC
    src/application.cpp
    src/ik_fk_key_admission.cpp
    src/gltf_scene_import.cpp
    src/shared_rig.cpp
    src/material_library.cpp
    src/object_aging.cpp
)

target_include_directories(carto_application
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_application
    PUBLIC
        carto_core
        carto_condition_authoring
        carto_connection
        carto_engineering
        carto_geometry
        carto_journal
        carto_project
        carto_io
        carto_editor
        carto_production
        carto_render
        carto_terrain
        carto_workspace
    PRIVATE
        carto_mesh_attributes
        carto_io
)

carto_target(carto_application)

install(TARGETS carto_application EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
