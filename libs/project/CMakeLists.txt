include("${_carto_public_root}/cmake/CartographerSQLite3.cmake")
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/sqlite_authoring_database.cpp"
    PROPERTY INCLUDE_DIRECTORIES "${CARTO_SQLITE3_INCLUDE_DIRECTORIES}")
if(CARTO_USE_WINSQLITE3)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/sqlite_authoring_database.cpp"
        APPEND PROPERTY COMPILE_DEFINITIONS
            CARTO_USE_WINSQLITE3=1
            NTDDI_VERSION=0x0A00000E)
endif()
if(CARTO_BUILD_TESTS)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/sqlite_authoring_database.cpp"
        APPEND PROPERTY COMPILE_DEFINITIONS CARTO_SQLITE_TEST_FAULT_INJECTION=1)
endif()

add_library(carto_project STATIC
    src/authoring_standards.cpp
    src/file_lock.cpp
    src/material_graph.cpp
    src/material_catalog.cpp
    src/package.cpp
    src/presentation_catalog.cpp
    src/project.cpp
    src/recovery.cpp
    src/road_source.cpp
    src/sqlite_authoring_database.cpp
    src/surface_material_policy.cpp
    src/surface_policy_admission.cpp
    src/transaction.cpp
)

target_include_directories(carto_project
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_project
    PUBLIC
        carto_core carto_scene carto_geometry carto_assets carto_surface_core
        carto_biological carto_spatial_field carto_condition_authoring
        carto_engineering carto_render carto_scientific carto_world
        carto_journal carto_eval
    PRIVATE
        CartoSQLite3 carto_character carto_mesh_attributes
        carto_pose carto_weights carto_motion
)
carto_target(carto_project)

install(TARGETS carto_project EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
