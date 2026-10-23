add_library(carto_character STATIC
    src/autorig_analysis.cpp
    src/autorig_candidates.cpp
    src/autorig_constraints.cpp
    src/bindings.cpp
    src/constraints_record.cpp
    src/controls.cpp
    src/coordinate.cpp
    src/definition.cpp
    src/drivers.cpp
    src/ids.cpp
    src/invalidation.cpp
    src/manifest.cpp
    src/morph.cpp
    src/motion_document.cpp
    src/receipt.cpp
    src/rig_module.cpp
    src/rig_symmetry.cpp
    src/time.cpp
    src/weights_record.cpp
)

target_include_directories(carto_character
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_character
    PUBLIC
        carto_core
        carto_assets
        carto_geometry
        carto_kinematic
        carto_temporal
        carto_scene
)
carto_target(carto_character)

install(TARGETS carto_character EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
