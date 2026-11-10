add_library(carto_production STATIC
    src/authoring_material.cpp
    src/authoring_material_bake.cpp
    src/authoring_aging.cpp
    src/authoring_material_surface.cpp
    src/authoring_viewport.cpp
    src/authoring_shadow.cpp
    src/authoring_environment.cpp
    src/authoring_temporal.cpp
    src/artifact_store.cpp
    src/bake_graph.cpp
    src/bake_job.cpp
    src/bake_ray_worker.cpp
    src/curvature_bake.cpp
    src/capability_profile.cpp
    src/closure.cpp
    src/decision.cpp
    src/dependency.cpp
    src/decal.cpp
    src/garment_asset.cpp
    src/lookdev_draw_package.cpp
    src/lightmap_uv.cpp
    src/material_artifact.cpp
    src/preparation_job.cpp
    src/preparation_operation.cpp
    src/preparation_scope.cpp
    src/prepared_character.cpp
    src/prepared_character_publication.cpp
    src/prepared_publication.cpp
    src/prepared_scene.cpp
    src/recipe.cpp
    src/reimport.cpp
    src/render_mesh.cpp
    src/scalar_bake.cpp
    src/sculpt_authoring.cpp
    src/uv_island_pack.cpp
    src/uv_planar_projection.cpp
    src/uv_relax.cpp
)

target_include_directories(carto_production
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_production
    PUBLIC
        carto_core
        carto_assets
        carto_character
        carto_project
        carto_geometry
        carto_mesh_attributes
        carto_render
        carto_navgraph
        carto_surface_core
)
carto_target(carto_production)

install(TARGETS carto_production EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
