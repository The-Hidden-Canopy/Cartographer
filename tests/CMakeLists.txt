
add_executable(cartographer_agent_workbench_tests
    agent_workbench_tests.cpp
)
target_link_libraries(cartographer_agent_workbench_tests
    PRIVATE
        carto_agent
        carto_application
        carto_ui
)
carto_target(cartographer_agent_workbench_tests)
add_test(NAME cartographer_agent_workbench_tests COMMAND cartographer_agent_workbench_tests)

add_executable(cartographer_scene_query_tests
    scene_query_tests.cpp
)
target_link_libraries(cartographer_scene_query_tests
    PRIVATE
        carto_agent
        carto_application
)
carto_target(cartographer_scene_query_tests)
add_test(NAME cartographer_scene_query_tests COMMAND cartographer_scene_query_tests)

add_executable(cartographer_tests
    test_main.cpp
)

add_executable(cartographer_hydro_tests
    hydro_tests.cpp
)
target_link_libraries(cartographer_hydro_tests
    PRIVATE
        carto_hydro
)
carto_target(cartographer_hydro_tests)
add_test(NAME cartographer_hydro_tests COMMAND cartographer_hydro_tests)

add_executable(cartographer_curve_path_tests
    curve_path_tests.cpp
)
target_link_libraries(cartographer_curve_path_tests
    PRIVATE
        carto_geometry
)
carto_target(cartographer_curve_path_tests)
add_test(NAME cartographer_curve_path_tests COMMAND cartographer_curve_path_tests)

add_executable(cartographer_application_tests
    application_tests.cpp
)

add_executable(cartographer_render_architecture_tests
    render_architecture_tests.cpp
)

add_executable(cartographer_kernel_evidence_tests
    kernel_evidence_tests.cpp
)
target_link_libraries(cartographer_kernel_evidence_tests
    PRIVATE
        carto_device
        carto_gpu
)
carto_target(cartographer_kernel_evidence_tests)
add_test(NAME cartographer_kernel_evidence_tests COMMAND cartographer_kernel_evidence_tests)

add_executable(cartographer_evaluation_attributes_tests
    evaluation_attributes_tests.cpp
)
target_link_libraries(cartographer_evaluation_attributes_tests
    PRIVATE
        carto_eval
        carto_attributes
        carto_mesh_attributes
)
carto_target(cartographer_evaluation_attributes_tests)
add_test(NAME cartographer_evaluation_attributes_tests COMMAND cartographer_evaluation_attributes_tests)

add_executable(cartographer_production_closure_tests
    production_closure_tests.cpp
)
target_link_libraries(cartographer_production_closure_tests
    PRIVATE
        carto_production
)
carto_target(cartographer_production_closure_tests)
add_test(NAME cartographer_production_closure_tests COMMAND cartographer_production_closure_tests)


add_executable(cartographer_biological_authoring_tests
    biological_authoring_tests.cpp
)
target_link_libraries(cartographer_biological_authoring_tests
    PRIVATE
        carto_biological
)
carto_target(cartographer_biological_authoring_tests)
add_test(NAME cartographer_biological_authoring_tests
    COMMAND cartographer_biological_authoring_tests)

add_executable(cartographer_obj_uv_tests obj_uv_tests.cpp)
target_link_libraries(cartographer_obj_uv_tests PRIVATE carto_io)
carto_target(cartographer_obj_uv_tests)
add_test(NAME cartographer_obj_uv_tests COMMAND cartographer_obj_uv_tests)

add_executable(cartographer_biological_provider_contract_tests
    biological_provider_contract_tests.cpp
)
target_link_libraries(cartographer_biological_provider_contract_tests
    PRIVATE
        carto_biological
)
carto_target(cartographer_biological_provider_contract_tests)
add_test(NAME cartographer_biological_provider_contract_tests
    COMMAND cartographer_biological_provider_contract_tests)

add_executable(cartographer_production_authoring_tests
    production_authoring_tests.cpp
)
target_link_libraries(cartographer_production_authoring_tests
    PRIVATE
        carto_production_authoring
)
carto_target(cartographer_production_authoring_tests)
add_test(NAME cartographer_production_authoring_tests
    COMMAND cartographer_production_authoring_tests)

add_executable(cartographer_engineering_tests
    engineering_tests.cpp
)
target_link_libraries(cartographer_engineering_tests
    PRIVATE
        carto_engineering
        carto_application
        carto_ui
)
carto_target(cartographer_engineering_tests)
add_test(NAME cartographer_engineering_tests COMMAND cartographer_engineering_tests)

add_executable(cartographer_drafting_tests
    drafting_tests.cpp
)
target_link_libraries(cartographer_drafting_tests PRIVATE carto_engineering)
carto_target(cartographer_drafting_tests)
add_test(NAME cartographer_drafting_tests COMMAND cartographer_drafting_tests)

add_executable(cartographer_condition_authoring_tests
    condition_authoring_tests.cpp
)
target_link_libraries(cartographer_condition_authoring_tests
    PRIVATE
        carto_condition_authoring
        carto_application
        carto_ui
)
carto_target(cartographer_condition_authoring_tests)
add_test(NAME cartographer_condition_authoring_tests
    COMMAND cartographer_condition_authoring_tests)

add_executable(cartographer_utility_network_tests
    utility_network_tests.cpp
)
target_link_libraries(cartographer_utility_network_tests PRIVATE carto_condition_authoring)
carto_target(cartographer_utility_network_tests)
add_test(NAME cartographer_utility_network_tests COMMAND cartographer_utility_network_tests)

add_executable(cartographer_district_topology_tests
    district_topology_tests.cpp
)
target_link_libraries(cartographer_district_topology_tests PRIVATE carto_condition_authoring)
carto_target(cartographer_district_topology_tests)
add_test(NAME cartographer_district_topology_tests COMMAND cartographer_district_topology_tests)

add_executable(cartographer_site_parcel_tests
    site_parcel_tests.cpp
)
target_link_libraries(cartographer_site_parcel_tests PRIVATE carto_condition_authoring)
carto_target(cartographer_site_parcel_tests)
add_test(NAME cartographer_site_parcel_tests COMMAND cartographer_site_parcel_tests)

add_executable(cartographer_condition_authoring_ui_fixture_tests
    condition_authoring_ui_fixture_tests.cpp
)
target_link_libraries(cartographer_condition_authoring_ui_fixture_tests
    PRIVATE
        carto_application
        carto_assets
        carto_biological
        carto_condition_authoring
        carto_project
        carto_ui
)
carto_target(cartographer_condition_authoring_ui_fixture_tests)
add_test(
    NAME cartographer_condition_authoring_ui_fixture_tests
    COMMAND cartographer_condition_authoring_ui_fixture_tests
        "${CMAKE_CURRENT_BINARY_DIR}/condition-authoring-ui-fixtures"
)

add_executable(cartographer_condition_pickup_fixture_tests
    condition_pickup_fixture_tests.cpp
)
target_link_libraries(cartographer_condition_pickup_fixture_tests
    PRIVATE
        carto_application
        carto_assets
        carto_condition_authoring
        carto_geometry
        carto_io
        carto_kinematic
        carto_project
        carto_render
        carto_ui
)
carto_target(cartographer_condition_pickup_fixture_tests)
add_test(
    NAME cartographer_condition_pickup_fixture_tests
    COMMAND cartographer_condition_pickup_fixture_tests
        "${CMAKE_BINARY_DIR}/p"
)

add_executable(cartographer_production_decision_tests
    production_decision_tests.cpp
)
target_link_libraries(cartographer_production_decision_tests
    PRIVATE
        carto_production
)
carto_target(cartographer_production_decision_tests)
add_test(NAME cartographer_production_decision_tests
    COMMAND cartographer_production_decision_tests)

add_executable(cartographer_world_model_tests
    world_model_tests.cpp
)
target_link_libraries(cartographer_world_model_tests
    PRIVATE
        carto_world
        carto_project
        carto_journal
)
carto_target(cartographer_world_model_tests)
add_test(NAME cartographer_world_model_tests COMMAND cartographer_world_model_tests)

add_executable(cartographer_material_catalog_tests
    material_catalog_tests.cpp
)
target_link_libraries(cartographer_material_catalog_tests
    PRIVATE
        carto_geometry
        carto_project
        carto_journal
)
carto_target(cartographer_material_catalog_tests)
add_test(NAME cartographer_material_catalog_tests COMMAND cartographer_material_catalog_tests)

add_executable(cartographer_material_graph_tests
    material_graph_tests.cpp
)
target_link_libraries(cartographer_material_graph_tests
    PRIVATE
        carto_project
)
carto_target(cartographer_material_graph_tests)
add_test(NAME cartographer_material_graph_tests COMMAND cartographer_material_graph_tests)

add_executable(cartographer_presentation_catalog_tests
    presentation_catalog_tests.cpp
)
target_link_libraries(cartographer_presentation_catalog_tests
    PRIVATE
        carto_project
        carto_journal
)
carto_target(cartographer_presentation_catalog_tests)
add_test(NAME cartographer_presentation_catalog_tests COMMAND cartographer_presentation_catalog_tests)

add_executable(cartographer_curve_path_project_tests
    curve_path_project_tests.cpp
)
target_link_libraries(cartographer_curve_path_project_tests
    PRIVATE
        carto_project
        carto_journal
)
carto_target(cartographer_curve_path_project_tests)
add_test(NAME cartographer_curve_path_project_tests COMMAND cartographer_curve_path_project_tests)

add_executable(cartographer_road_source_tests
    road_source_tests.cpp
)
target_link_libraries(cartographer_road_source_tests
    PRIVATE
        carto_geometry
        carto_project
        carto_journal
)
carto_target(cartographer_road_source_tests)
add_test(NAME cartographer_road_source_tests COMMAND cartographer_road_source_tests)

add_executable(cartographer_procedural_sweep_tests
    procedural_sweep_tests.cpp
)
target_link_libraries(cartographer_procedural_sweep_tests PRIVATE carto_geometry)
carto_target(cartographer_procedural_sweep_tests)
add_test(NAME cartographer_procedural_sweep_tests COMMAND cartographer_procedural_sweep_tests)

add_executable(cartographer_private_runtime_client_tests
    private_runtime_client_tests.cpp
)
target_link_libraries(cartographer_private_runtime_client_tests PRIVATE
    carto_runtime_api
)
carto_target(cartographer_private_runtime_client_tests)
add_test(NAME cartographer_private_runtime_client_tests
    COMMAND cartographer_private_runtime_client_tests)

add_executable(cartographer_generated_gltf_import_tests
    generated_gltf_import_tests.cpp
)
target_link_libraries(cartographer_generated_gltf_import_tests PRIVATE carto_io)
carto_target(cartographer_generated_gltf_import_tests)
add_test(NAME cartographer_generated_gltf_import_tests
    COMMAND cartographer_generated_gltf_import_tests)

add_executable(cartographer_sign_geometry_tests
    sign_geometry_tests.cpp
)
target_link_libraries(cartographer_sign_geometry_tests PRIVATE carto_geometry)
carto_target(cartographer_sign_geometry_tests)
add_test(NAME cartographer_sign_geometry_tests COMMAND cartographer_sign_geometry_tests)

add_executable(cartographer_sign_text_project_tests
    sign_text_project_tests.cpp
)
target_link_libraries(cartographer_sign_text_project_tests
    PRIVATE
        carto_geometry
        carto_project
        carto_journal
)
carto_target(cartographer_sign_text_project_tests)
add_test(NAME cartographer_sign_text_project_tests
    COMMAND cartographer_sign_text_project_tests)

add_executable(cartographer_sweep_source_project_tests
    sweep_source_project_tests.cpp
)
target_link_libraries(cartographer_sweep_source_project_tests
    PRIVATE
        carto_geometry
        carto_project
        carto_journal
)
carto_target(cartographer_sweep_source_project_tests)
add_test(NAME cartographer_sweep_source_project_tests
    COMMAND cartographer_sweep_source_project_tests)

add_executable(cartographer_sweep_endpoint_tests
    sweep_endpoint_tests.cpp
)
target_link_libraries(cartographer_sweep_endpoint_tests
    PRIVATE
        carto_condition_authoring
)
carto_target(cartographer_sweep_endpoint_tests)
add_test(NAME cartographer_sweep_endpoint_tests COMMAND cartographer_sweep_endpoint_tests)

add_executable(cartographer_biological_project_tests
    biological_project_tests.cpp
)
target_link_libraries(cartographer_biological_project_tests
    PRIVATE
        carto_biological
        carto_project
        carto_journal
)
carto_target(cartographer_biological_project_tests)
add_test(NAME cartographer_biological_project_tests
    COMMAND cartographer_biological_project_tests)

add_executable(cartographer_biological_field_visualization_tests
    biological_field_visualization_tests.cpp
)
target_link_libraries(cartographer_biological_field_visualization_tests
    PRIVATE carto_biological)
carto_target(cartographer_biological_field_visualization_tests)
add_test(NAME cartographer_biological_field_visualization_tests
    COMMAND cartographer_biological_field_visualization_tests)

add_executable(cartographer_texture_channel_pack_tests
    texture_channel_pack_tests.cpp
)
target_link_libraries(cartographer_texture_channel_pack_tests PRIVATE carto_assets)
carto_target(cartographer_texture_channel_pack_tests)
add_test(NAME cartographer_texture_channel_pack_tests
    COMMAND cartographer_texture_channel_pack_tests)

add_executable(cartographer_texture_atlas_tests
    texture_atlas_tests.cpp
)
target_link_libraries(cartographer_texture_atlas_tests PRIVATE carto_assets)
carto_target(cartographer_texture_atlas_tests)
add_test(NAME cartographer_texture_atlas_tests
    COMMAND cartographer_texture_atlas_tests)

add_executable(cartographer_texture_paint_tests
    texture_paint_tests.cpp
)
target_link_libraries(cartographer_texture_paint_tests PRIVATE carto_assets)
carto_target(cartographer_texture_paint_tests)
add_test(NAME cartographer_texture_paint_tests
    COMMAND cartographer_texture_paint_tests)

add_executable(cartographer_asset_lock_tests
    asset_lock_tests.cpp
)
target_link_libraries(cartographer_asset_lock_tests PRIVATE carto_assets)
carto_target(cartographer_asset_lock_tests)
add_test(NAME cartographer_asset_lock_tests COMMAND cartographer_asset_lock_tests)

add_executable(cartographer_spatial_field_tests
    spatial_field_tests.cpp
)
target_link_libraries(cartographer_spatial_field_tests
    PRIVATE
        carto_spatial_field
        carto_project
        carto_journal
)
carto_target(cartographer_spatial_field_tests)
add_test(NAME cartographer_spatial_field_tests COMMAND cartographer_spatial_field_tests)

add_executable(cartographer_terrain_tests
    terrain_tests.cpp
)
target_link_libraries(cartographer_terrain_tests
    PRIVATE
        carto_terrain
)
carto_target(cartographer_terrain_tests)
add_test(NAME cartographer_terrain_tests COMMAND cartographer_terrain_tests)

set(_carto_package_consumer_vulkan_args)
if(CARTO_ENABLE_VULKAN AND NOT CARTO_VULKAN_SDK_ROOT STREQUAL "")
    list(APPEND _carto_package_consumer_vulkan_args
        "-DCARTO_VULKAN_ROOT=${CARTO_VULKAN_SDK_ROOT}")
endif()

add_test(
    NAME cartographer_package_consumer_tests
    COMMAND
        ${CMAKE_COMMAND}
        "-DCARTO_BINARY_DIR=${CMAKE_BINARY_DIR}"
        "-DCARTO_CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/package_consumer"
        "-DCARTO_CONFIG=$<CONFIG>"
        "-DCARTO_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
        "-DCARTO_GENERATOR=${CMAKE_GENERATOR}"
        "-DCARTO_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DCARTO_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        "-DCARTO_CTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
        ${_carto_package_consumer_vulkan_args}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/RunPackageConsumer.cmake"
)
set_tests_properties(cartographer_package_consumer_tests PROPERTIES RUN_SERIAL TRUE)
unset(_carto_package_consumer_vulkan_args)

add_executable(cartographer_texture_source_tests
    texture_source_tests.cpp
)
target_link_libraries(cartographer_texture_source_tests
    PRIVATE
        carto_assets
)
carto_target(cartographer_texture_source_tests)
add_test(NAME cartographer_texture_source_tests COMMAND cartographer_texture_source_tests)

add_executable(cartographer_material_artifact_tests
    material_artifact_tests.cpp
)
target_link_libraries(cartographer_material_artifact_tests
    PRIVATE
        carto_production
)
carto_target(cartographer_material_artifact_tests)
add_test(NAME cartographer_material_artifact_tests COMMAND cartographer_material_artifact_tests)

add_executable(cartographer_bake_graph_tests
    bake_graph_tests.cpp
)
target_link_libraries(cartographer_bake_graph_tests
    PRIVATE
        carto_production
)
carto_target(cartographer_bake_graph_tests)
add_test(NAME cartographer_bake_graph_tests COMMAND cartographer_bake_graph_tests)

add_executable(cartographer_bake_tile_incremental_tests
    bake_tile_incremental_tests.cpp
)
target_link_libraries(cartographer_bake_tile_incremental_tests
    PRIVATE
        carto_production
)
carto_target(cartographer_bake_tile_incremental_tests)
add_test(NAME cartographer_bake_tile_incremental_tests
    COMMAND cartographer_bake_tile_incremental_tests)

add_executable(cartographer_bake_job_tests
    bake_job_tests.cpp
)
target_link_libraries(cartographer_bake_job_tests
    PRIVATE
        carto_production
)
carto_target(cartographer_bake_job_tests)
add_test(NAME cartographer_bake_job_tests COMMAND cartographer_bake_job_tests)

add_executable(cartographer_bake_ray_worker_tests
    bake_ray_worker_tests.cpp
)
target_link_libraries(cartographer_bake_ray_worker_tests
    PRIVATE
        carto_production
)
carto_target(cartographer_bake_ray_worker_tests)
add_test(NAME cartographer_bake_ray_worker_tests COMMAND cartographer_bake_ray_worker_tests)

add_executable(cartographer_lightmap_uv_tests
    lightmap_uv_tests.cpp
)
target_link_libraries(cartographer_lightmap_uv_tests
    PRIVATE
        carto_production
        carto_project
        carto_journal
)
carto_target(cartographer_lightmap_uv_tests)
add_test(NAME cartographer_lightmap_uv_tests COMMAND cartographer_lightmap_uv_tests)

add_executable(cartographer_uv_island_pack_tests
    uv_island_pack_tests.cpp
)
target_link_libraries(cartographer_uv_island_pack_tests
    PRIVATE
        carto_production
        carto_project
        carto_journal
)
carto_target(cartographer_uv_island_pack_tests)
add_test(NAME cartographer_uv_island_pack_tests COMMAND cartographer_uv_island_pack_tests)

add_executable(cartographer_garment_asset_tests
    garment_asset_tests.cpp
)
target_link_libraries(cartographer_garment_asset_tests
    PRIVATE
        carto_production
        carto_project
        carto_journal
)
carto_target(cartographer_garment_asset_tests)
add_test(NAME cartographer_garment_asset_tests COMMAND cartographer_garment_asset_tests)

if(TARGET cartographer_cli)
    add_executable(cartographer_cli_mesh_fixture
        cli_mesh_fixture.cpp
    )
    target_link_libraries(cartographer_cli_mesh_fixture
        PRIVATE
            carto_attributes
            carto_project
            carto_journal
    )
    carto_target(cartographer_cli_mesh_fixture)
    add_dependencies(cartographer_cli_mesh_fixture cartographer_cli)
    add_test(
        NAME cartographer_gltf_glb_cli_export_tests
        COMMAND
            ${CMAKE_COMMAND}
            "-DCARTO_CLI=$<TARGET_FILE:cartographer_cli>"
            "-DCARTO_FIXTURE=$<TARGET_FILE:cartographer_cli_mesh_fixture>"
            "-DCARTO_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_gltf_cli_export_tests.cmake"
    )
    set_tests_properties(cartographer_gltf_glb_cli_export_tests PROPERTIES RUN_SERIAL TRUE)
    add_test(
        NAME cartographer_ao_cli_tests
        COMMAND
            ${CMAKE_COMMAND}
            "-DCARTO_CLI=$<TARGET_FILE:cartographer_cli>"
            "-DCARTO_FIXTURE=$<TARGET_FILE:cartographer_cli_mesh_fixture>"
            "-DCARTO_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            "-DCARTO_ENABLE_D3D12=${CARTO_ENABLE_D3D12}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_ao_cli_tests.cmake"
    )
    set_tests_properties(cartographer_ao_cli_tests PROPERTIES RUN_SERIAL TRUE)
    add_test(
        NAME cartographer_bake_normal_cli_tests
        COMMAND
            ${CMAKE_COMMAND}
            "-DCARTO_CLI=$<TARGET_FILE:cartographer_cli>"
            "-DCARTO_FIXTURE=$<TARGET_FILE:cartographer_cli_mesh_fixture>"
            "-DCARTO_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_bake_normal_cli_tests.cmake"
    )
    set_tests_properties(cartographer_bake_normal_cli_tests PROPERTIES RUN_SERIAL TRUE)
endif()

add_executable(cartographer_lookdev_draw_package_tests
    lookdev_draw_package_tests.cpp
)
target_link_libraries(cartographer_lookdev_draw_package_tests
    PRIVATE
        carto_production
)
carto_target(cartographer_lookdev_draw_package_tests)
add_test(NAME cartographer_lookdev_draw_package_tests
    COMMAND cartographer_lookdev_draw_package_tests)

add_executable(cartographer_prepared_scene_tests
    prepared_scene_tests.cpp
)
target_link_libraries(cartographer_prepared_scene_tests
    PRIVATE
        carto_production
        carto_journal
)
carto_target(cartographer_prepared_scene_tests)
add_test(NAME cartographer_prepared_scene_tests COMMAND cartographer_prepared_scene_tests)

add_executable(cartographer_preparation_scope_tests
    preparation_scope_tests.cpp
)
target_link_libraries(cartographer_preparation_scope_tests
    PRIVATE
        carto_production
        carto_journal
)
carto_target(cartographer_preparation_scope_tests)
add_test(NAME cartographer_preparation_scope_tests COMMAND cartographer_preparation_scope_tests)

add_executable(cartographer_prepared_publication_tests
    prepared_publication_tests.cpp
)
target_link_libraries(cartographer_prepared_publication_tests
    PRIVATE
        carto_production
)
carto_target(cartographer_prepared_publication_tests)
add_test(NAME cartographer_prepared_publication_tests
    COMMAND cartographer_prepared_publication_tests)

add_executable(cartographer_prepared_publication_compatibility_tests
    prepared_publication_compatibility_tests.cpp
)
target_link_libraries(cartographer_prepared_publication_compatibility_tests
    PRIVATE
        carto_production
)
carto_target(cartographer_prepared_publication_compatibility_tests)
add_test(
    NAME cartographer_prepared_publication_compatibility_tests
    COMMAND cartographer_prepared_publication_compatibility_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cartographer-destination-prepared-v1
)

add_executable(cartographer_preparation_operation_tests
    preparation_operation_tests.cpp
)
target_link_libraries(cartographer_preparation_operation_tests
    PRIVATE
        carto_production
        carto_journal
)
carto_target(cartographer_preparation_operation_tests)
add_test(NAME cartographer_preparation_operation_tests
    COMMAND cartographer_preparation_operation_tests)

add_executable(cartographer_preparation_job_tests
    preparation_job_tests.cpp
)
target_link_libraries(cartographer_preparation_job_tests
    PRIVATE
        carto_production
)
carto_target(cartographer_preparation_job_tests)
add_test(NAME cartographer_preparation_job_tests
    COMMAND cartographer_preparation_job_tests)

add_executable(cartographer_web_geometry_tests
    web_geometry_tests.cpp
)
target_link_libraries(cartographer_web_geometry_tests
    PRIVATE
        carto_web_geometry_compile
        carto_web_geometry_core
        carto_web_geometry_evaluation
        carto_web_geometry_package
        carto_eval
        carto_attributes
        carto_geometry
)
carto_target(cartographer_web_geometry_tests)
add_test(NAME cartographer_web_geometry_tests COMMAND cartographer_web_geometry_tests)

add_executable(cartographer_surface_closure_tests
    surface_closure_tests.cpp
)
target_link_libraries(cartographer_surface_closure_tests
    PRIVATE
        carto_surface_core
        carto_geometry
)
carto_target(cartographer_surface_closure_tests)
add_test(NAME cartographer_surface_closure_tests COMMAND cartographer_surface_closure_tests)

add_executable(cartographer_surface_material_policy_tests
    surface_material_policy_tests.cpp
)
target_link_libraries(cartographer_surface_material_policy_tests
    PRIVATE
        carto_project
        carto_surface_core
)
carto_target(cartographer_surface_material_policy_tests)
add_test(NAME cartographer_surface_material_policy_tests
    COMMAND cartographer_surface_material_policy_tests)

add_executable(cartographer_surface_policy_admission_tests
    surface_policy_admission_tests.cpp
)
target_link_libraries(cartographer_surface_policy_admission_tests
    PRIVATE
        carto_project
        carto_surface_core
)
carto_target(cartographer_surface_policy_admission_tests)
add_test(NAME cartographer_surface_policy_admission_tests
    COMMAND cartographer_surface_policy_admission_tests)

add_executable(cartographer_surface_fidelity_package_json_tests
    surface_fidelity_package_json_tests.cpp
)
target_link_libraries(cartographer_surface_fidelity_package_json_tests
    PRIVATE
        carto_surface_core
        carto_geometry
)
carto_target(cartographer_surface_fidelity_package_json_tests)
add_test(NAME cartographer_surface_fidelity_package_json_tests
    COMMAND cartographer_surface_fidelity_package_json_tests)

add_executable(cartographer_surface_workflow_tests
    surface_workflow_tests.cpp
)
target_link_libraries(cartographer_surface_workflow_tests
    PRIVATE
        carto_surface_core
)
carto_target(cartographer_surface_workflow_tests)
add_test(NAME cartographer_surface_workflow_tests COMMAND cartographer_surface_workflow_tests)

add_executable(cartographer_surface_spectrum_tests
    surface_spectrum_tests.cpp
)
target_link_libraries(cartographer_surface_spectrum_tests
    PRIVATE
        carto_surface_core
        carto_geometry
)
carto_target(cartographer_surface_spectrum_tests)
add_test(NAME cartographer_surface_spectrum_tests COMMAND cartographer_surface_spectrum_tests)

add_executable(cartographer_surface_distribution_tests
    surface_distribution_tests.cpp
)
target_link_libraries(cartographer_surface_distribution_tests
    PRIVATE
        carto_surface_core
)
carto_target(cartographer_surface_distribution_tests)
add_test(NAME cartographer_surface_distribution_tests COMMAND cartographer_surface_distribution_tests)

add_executable(cartographer_surface_qem_tests
    surface_qem_tests.cpp
)
target_link_libraries(cartographer_surface_qem_tests
    PRIVATE
        carto_surface_core
        carto_geometry
)
carto_target(cartographer_surface_qem_tests)
add_test(NAME cartographer_surface_qem_tests COMMAND cartographer_surface_qem_tests)

add_executable(cartographer_surface_normal_closure_tests
    surface_normal_closure_tests.cpp
)
target_link_libraries(cartographer_surface_normal_closure_tests
    PRIVATE
        carto_surface_core
        carto_geometry
)
carto_target(cartographer_surface_normal_closure_tests)
add_test(NAME cartographer_surface_normal_closure_tests COMMAND cartographer_surface_normal_closure_tests)

add_executable(cartographer_surface_visibility_closure_tests
    surface_visibility_closure_tests.cpp
)
target_link_libraries(cartographer_surface_visibility_closure_tests
    PRIVATE
        carto_surface_core
        carto_geometry
)
carto_target(cartographer_surface_visibility_closure_tests)
add_test(NAME cartographer_surface_visibility_closure_tests COMMAND cartographer_surface_visibility_closure_tests)

if(CARTO_BUILD_DEVICE_CPU)
    add_executable(cartographer_device_tests
        device_tests.cpp
    )
    target_link_libraries(cartographer_device_tests
        PRIVATE
            carto_device_cpu
            carto_device
            carto_device_ir
            carto_gpu
    )
    carto_target(cartographer_device_tests)
    add_test(NAME cartographer_device_tests COMMAND cartographer_device_tests)
endif()

if(CARTO_ENABLE_D3D12)
    add_executable(cartographer_d3d12_tests
        d3d12_tests.cpp
    )
    target_link_libraries(cartographer_d3d12_tests
        PRIVATE
            carto_d3d12
            carto_d3d12_bake
            carto_device
            carto_device_ir
            carto_gpu
    )
    target_compile_definitions(cartographer_d3d12_tests
        PRIVATE
            CARTO_D3D12_SHADER_DIR="${CMAKE_CURRENT_LIST_DIR}/../libs/d3d12/shaders"
    )
    if(WIN32)
        target_link_libraries(cartographer_d3d12_tests PRIVATE user32)
    endif()
    carto_target(cartographer_d3d12_tests)
    add_test(NAME cartographer_d3d12_tests COMMAND cartographer_d3d12_tests)

    add_executable(cartographer_d3d12_viewport_input_tests
        d3d12_viewport_input_tests.cpp
    )
    target_include_directories(cartographer_d3d12_viewport_input_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/libs/d3d12/include")
    carto_target(cartographer_d3d12_viewport_input_tests)
    add_test(NAME cartographer_d3d12_viewport_input_tests
        COMMAND cartographer_d3d12_viewport_input_tests)

    add_executable(cartographer_web_geometry_overlay_d3d12_tests
        web_geometry_overlay_d3d12_tests.cpp
    )
    target_link_libraries(cartographer_web_geometry_overlay_d3d12_tests
        PRIVATE
            carto_d3d12
            carto_device
            carto_device_ir
            carto_gpu
            carto_web_geometry_compile
            carto_web_geometry_core
            carto_web_geometry_package
            carto_attributes
            carto_geometry
    )
    target_compile_definitions(cartographer_web_geometry_overlay_d3d12_tests
        PRIVATE
            CARTO_VISUAL_BASELINE_DIR="${CMAKE_CURRENT_LIST_DIR}/visual_baselines"
    )
    carto_target(cartographer_web_geometry_overlay_d3d12_tests)
    add_test(NAME cartographer_web_geometry_overlay_d3d12_tests
        COMMAND cartographer_web_geometry_overlay_d3d12_tests)
endif()

add_executable(cartographer_storage_journal_tests
    storage_journal_tests.cpp
)

add_executable(cartographer_sdk_tests
    sdk_tests.cpp
)

add_executable(cartographer_sdk_c_smoke
    sdk_c_smoke.c
)

set_target_properties(cartographer_sdk_c_smoke PROPERTIES LINKER_LANGUAGE CXX)

add_executable(cartographer_provider_plugin_tests
    provider_plugin_tests.cpp
)

add_executable(cartographer_plugin_package_tests
    plugin_package_tests.cpp
)

target_link_libraries(cartographer_plugin_package_tests
    PRIVATE
        carto_plugin_package
)
target_include_directories(cartographer_plugin_package_tests
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../libs/plugin_package/src
)
carto_target(cartographer_plugin_package_tests)
add_test(NAME cartographer_plugin_package_tests COMMAND cartographer_plugin_package_tests)

add_executable(cartographer_ui_tests
    ui_tests.cpp
)

add_executable(cartographer_viewport_interaction_tests
    viewport_interaction_tests.cpp
)

add_executable(cartographer_observation_tests
    observation_tests.cpp
)
target_link_libraries(cartographer_observation_tests
    PRIVATE
        carto_editor
)
carto_target(cartographer_observation_tests)
add_test(NAME cartographer_observation_tests COMMAND cartographer_observation_tests)

add_executable(cartographer_public_api_tests
    public_api_tests.cpp
)

add_executable(cartographer_public_boundary_tests
    public_boundary_tests.cpp
)
target_link_libraries(cartographer_public_boundary_tests
    PRIVATE
        carto_production
)
carto_target(cartographer_public_boundary_tests)
add_test(NAME cartographer_public_boundary_tests COMMAND cartographer_public_boundary_tests)

add_executable(cartographer_preview_transaction_tests
    preview_transaction_tests.cpp
)

add_executable(cartographer_ai_tests
    ai_tests.cpp
)

add_executable(cartographer_topology_edit_tests
    topology_edit_tests.cpp
)

add_executable(cartographer_scientific_tests
    scientific_model_tests.cpp
)

target_link_libraries(cartographer_scientific_tests
    PRIVATE
        carto_scientific
        carto_project
        carto_journal
)

carto_target(cartographer_scientific_tests)
add_test(NAME cartographer_scientific_tests COMMAND cartographer_scientific_tests)


target_link_libraries(cartographer_tests
    PRIVATE
        carto_core
        carto_scene
        carto_geometry
        carto_project
        carto_editor
        carto_render
        carto_application
        carto_io
)

carto_target(cartographer_tests)
add_test(NAME cartographer_tests COMMAND cartographer_tests)
target_link_libraries(cartographer_application_tests
    PRIVATE
        carto_application
        carto_journal
)

carto_target(cartographer_application_tests)
add_test(NAME cartographer_application_tests COMMAND cartographer_application_tests)

target_link_libraries(cartographer_render_architecture_tests
    PRIVATE
        carto_core
        carto_assets
        carto_gpu
        carto_render
        carto_render_graph
)

carto_target(cartographer_render_architecture_tests)
add_test(NAME cartographer_render_architecture_tests COMMAND cartographer_render_architecture_tests)

add_executable(cartographer_uv_material_contract_tests
    uv_material_contract_tests.cpp
)
target_link_libraries(cartographer_uv_material_contract_tests
    PRIVATE
        carto_attributes
        carto_geometry
        carto_io
        carto_project
        carto_render
)
carto_target(cartographer_uv_material_contract_tests)
add_test(NAME cartographer_uv_material_contract_tests COMMAND cartographer_uv_material_contract_tests)

if(CARTO_ENABLE_VULKAN)
    add_executable(cartographer_vulkan_runtime_tests
        vulkan_runtime_tests.cpp
    )
    target_link_libraries(cartographer_vulkan_runtime_tests PRIVATE carto_vulkan)
    carto_target(cartographer_vulkan_runtime_tests)
    add_test(NAME cartographer_vulkan_runtime_tests COMMAND cartographer_vulkan_runtime_tests)
endif()

target_link_libraries(cartographer_storage_journal_tests
    PRIVATE
        carto_core
        carto_assets
        carto_journal
        carto_project
        carto_eval
)

carto_target(cartographer_storage_journal_tests)
add_test(NAME cartographer_storage_journal_tests COMMAND cartographer_storage_journal_tests)

target_link_libraries(cartographer_sdk_tests
    PRIVATE
        carto_sdk
)

carto_target(cartographer_sdk_tests)
add_test(NAME cartographer_sdk_tests COMMAND cartographer_sdk_tests)

target_link_libraries(cartographer_sdk_c_smoke
    PRIVATE
        carto_sdk
)

add_test(NAME cartographer_sdk_c_smoke COMMAND cartographer_sdk_c_smoke)

target_link_libraries(cartographer_provider_plugin_tests
    PRIVATE
        carto_core
        carto_io
        carto_providers
        carto_plugin_protocol
)

carto_target(cartographer_provider_plugin_tests)
if(WIN32)
    add_executable(cartographer_provider_process_child
        provider_process_child.c
    )
    set_target_properties(cartographer_provider_process_child PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
    )
    add_dependencies(cartographer_provider_plugin_tests
        cartographer_provider_process_child
    )
    add_test(NAME cartographer_provider_plugin_tests
        COMMAND cartographer_provider_plugin_tests
            $<TARGET_FILE:cartographer_provider_process_child>
    )
else()
    add_test(NAME cartographer_provider_plugin_tests COMMAND cartographer_provider_plugin_tests)
endif()

target_link_libraries(cartographer_ui_tests
    PRIVATE
        carto_application
        carto_ui
        carto_runtime_api
)

carto_target(cartographer_ui_tests)
add_test(NAME cartographer_ui_tests COMMAND cartographer_ui_tests)

if(TARGET carto_imgui)
    add_executable(cartographer_desktop_workbench_input_tests
        desktop_workbench_input_tests.cpp
    )
    target_link_libraries(cartographer_desktop_workbench_input_tests
        PRIVATE
            carto_imgui
            carto_ui
    )
    carto_target(cartographer_desktop_workbench_input_tests)
    add_test(NAME cartographer_desktop_workbench_input_tests
        COMMAND cartographer_desktop_workbench_input_tests)
endif()

target_link_libraries(cartographer_viewport_interaction_tests
    PRIVATE
        carto_editor
)
carto_target(cartographer_viewport_interaction_tests)
add_test(NAME cartographer_viewport_interaction_tests
    COMMAND cartographer_viewport_interaction_tests)

add_executable(cartographer_editor_camera_tests
    editor_camera_tests.cpp
)
target_link_libraries(cartographer_editor_camera_tests
    PRIVATE
        carto_editor
)
carto_target(cartographer_editor_camera_tests)
add_test(NAME cartographer_editor_camera_tests
    COMMAND cartographer_editor_camera_tests)

add_executable(cartographer_snap_service_tests
    snap_service_tests.cpp
)
target_link_libraries(cartographer_snap_service_tests
    PRIVATE
        carto_editor
)
carto_target(cartographer_snap_service_tests)
add_test(NAME cartographer_snap_service_tests
    COMMAND cartographer_snap_service_tests)

target_link_libraries(cartographer_public_api_tests
    PRIVATE
        carto_application
        carto_editor
        carto_mesh_attributes
        carto_production
        carto_project
)

carto_target(cartographer_public_api_tests)
add_test(NAME cartographer_public_api_tests COMMAND cartographer_public_api_tests)

target_link_libraries(cartographer_preview_transaction_tests
    PRIVATE
        carto_application
        carto_editor
        carto_ui
)

carto_target(cartographer_preview_transaction_tests)
add_test(NAME cartographer_preview_transaction_tests COMMAND cartographer_preview_transaction_tests)

target_link_libraries(cartographer_ai_tests
    PRIVATE
        carto_ai
        carto_ui
        carto_application
)

carto_target(cartographer_ai_tests)
add_test(NAME cartographer_ai_tests COMMAND cartographer_ai_tests)

target_link_libraries(cartographer_topology_edit_tests
    PRIVATE
        carto_application
        carto_editor
        carto_geometry
        carto_mesh_attributes
)

carto_target(cartographer_topology_edit_tests)
add_test(NAME cartographer_topology_edit_tests COMMAND cartographer_topology_edit_tests)

add_executable(cartographer_reimport_tests
    reimport_tests.cpp
)
target_link_libraries(cartographer_reimport_tests
    PRIVATE
        carto_production
)
carto_target(cartographer_reimport_tests)
add_test(NAME cartographer_reimport_tests COMMAND cartographer_reimport_tests)

add_executable(cartographer_production_capability_profile_tests
    production_capability_profile_tests.cpp
)
target_link_libraries(cartographer_production_capability_profile_tests
    PRIVATE
        carto_production
)
carto_target(cartographer_production_capability_profile_tests)
add_test(NAME cartographer_production_capability_profile_tests
    COMMAND cartographer_production_capability_profile_tests)

add_executable(cartographer_production_recipe_tests
    production_recipe_tests.cpp
)
target_link_libraries(cartographer_production_recipe_tests
    PRIVATE
        carto_production
)
carto_target(cartographer_production_recipe_tests)
add_test(NAME cartographer_production_recipe_tests
    COMMAND cartographer_production_recipe_tests)

add_executable(cartographer_decal_tests
    decal_tests.cpp
)
target_link_libraries(cartographer_decal_tests PRIVATE carto_production)
carto_target(cartographer_decal_tests)
add_test(NAME cartographer_decal_tests COMMAND cartographer_decal_tests)

add_executable(cartographer_semantic_sidecar_tests
    semantic_sidecar_tests.cpp
)
target_link_libraries(cartographer_semantic_sidecar_tests
    PRIVATE
        carto_semantic
        carto_project
        carto_journal
)
carto_target(cartographer_semantic_sidecar_tests)
add_test(NAME cartographer_semantic_sidecar_tests
    COMMAND cartographer_semantic_sidecar_tests)

add_executable(cartographer_semantic_schema_tests
    semantic_schema_tests.cpp
)
target_link_libraries(cartographer_semantic_schema_tests
    PRIVATE
        carto_semantic
)
carto_target(cartographer_semantic_schema_tests)
add_test(NAME cartographer_semantic_schema_tests
    COMMAND cartographer_semantic_schema_tests)

add_executable(cartographer_semantic_lowering_tests
    semantic_lowering_tests.cpp
)
target_link_libraries(cartographer_semantic_lowering_tests
    PRIVATE
        carto_semantic
)
carto_target(cartographer_semantic_lowering_tests)
add_test(NAME cartographer_semantic_lowering_tests
    COMMAND cartographer_semantic_lowering_tests)

add_executable(cartographer_semantic_migration_tests
    semantic_migration_tests.cpp
)
target_link_libraries(cartographer_semantic_migration_tests
    PRIVATE
        carto_semantic
)
carto_target(cartographer_semantic_migration_tests)
add_test(NAME cartographer_semantic_migration_tests
    COMMAND cartographer_semantic_migration_tests)

add_executable(cartographer_semantic_query_tests
    semantic_query_tests.cpp
)
target_link_libraries(cartographer_semantic_query_tests
    PRIVATE
        carto_semantic
        carto_project
        carto_journal
)
carto_target(cartographer_semantic_query_tests)
add_test(NAME cartographer_semantic_query_tests
    COMMAND cartographer_semantic_query_tests)

add_executable(cartographer_connection_port_tests
    connection_port_tests.cpp
)
target_link_libraries(cartographer_connection_port_tests
    PRIVATE
        carto_connection
)
carto_target(cartographer_connection_port_tests)
add_test(NAME cartographer_connection_port_tests
    COMMAND cartographer_connection_port_tests)

add_executable(cartographer_connection_port_graph_tests
    connection_port_graph_tests.cpp
)
target_link_libraries(cartographer_connection_port_graph_tests
    PRIVATE
        carto_connection
)
carto_target(cartographer_connection_port_graph_tests)
add_test(NAME cartographer_connection_port_graph_tests
    COMMAND cartographer_connection_port_graph_tests)

add_executable(cartographer_physical_mass_tests
    physical_mass_tests.cpp
)
target_link_libraries(cartographer_physical_mass_tests
    PRIVATE
        carto_physical
)
carto_target(cartographer_physical_mass_tests)
add_test(NAME cartographer_physical_mass_tests
    COMMAND cartographer_physical_mass_tests)

add_executable(cartographer_physical_twin_tests
    physical_twin_tests.cpp
)
target_link_libraries(cartographer_physical_twin_tests
    PRIVATE
        carto_physical
)
carto_target(cartographer_physical_twin_tests)
add_test(NAME cartographer_physical_twin_tests
    COMMAND cartographer_physical_twin_tests)

add_executable(cartographer_surface_ledger_tests
    surface_ledger_tests.cpp
)
target_link_libraries(cartographer_surface_ledger_tests
    PRIVATE
        carto_physical
)
carto_target(cartographer_surface_ledger_tests)
add_test(NAME cartographer_surface_ledger_tests
    COMMAND cartographer_surface_ledger_tests)

add_executable(cartographer_collision_candidate_tests
    collision_candidate_tests.cpp
)
target_link_libraries(cartographer_collision_candidate_tests
    PRIVATE
        carto_collision
)
carto_target(cartographer_collision_candidate_tests)
add_test(NAME cartographer_collision_candidate_tests
    COMMAND cartographer_collision_candidate_tests)

add_executable(cartographer_collision_policy_tests
    collision_policy_tests.cpp
)
target_link_libraries(cartographer_collision_policy_tests
    PRIVATE
        carto_collision
)
carto_target(cartographer_collision_policy_tests)
add_test(NAME cartographer_collision_policy_tests
    COMMAND cartographer_collision_policy_tests)

add_executable(cartographer_navgraph_tests
    navgraph_tests.cpp
)
target_link_libraries(cartographer_navgraph_tests
    PRIVATE
        carto_navgraph
        carto_assets
        carto_production
)
carto_target(cartographer_navgraph_tests)
add_test(NAME cartographer_navgraph_tests COMMAND cartographer_navgraph_tests)

add_executable(cartographer_navgraph_agent_profile_tests
    navgraph_agent_profile_tests.cpp
)
target_link_libraries(cartographer_navgraph_agent_profile_tests
    PRIVATE
        carto_navgraph
        carto_assets
)
carto_target(cartographer_navgraph_agent_profile_tests)
add_test(NAME cartographer_navgraph_agent_profile_tests
    COMMAND cartographer_navgraph_agent_profile_tests)

add_executable(cartographer_kinematic_graph_tests
    kinematic_graph_tests.cpp
)
target_link_libraries(cartographer_kinematic_graph_tests
    PRIVATE
        carto_kinematic
)
carto_target(cartographer_kinematic_graph_tests)
add_test(NAME cartographer_kinematic_graph_tests
    COMMAND cartographer_kinematic_graph_tests)

add_executable(cartographer_kinematic_identity_tests
    kinematic_identity_tests.cpp
)
target_link_libraries(cartographer_kinematic_identity_tests
    PRIVATE
        carto_kinematic
)
carto_target(cartographer_kinematic_identity_tests)
add_test(NAME cartographer_kinematic_identity_tests
    COMMAND cartographer_kinematic_identity_tests)

add_executable(cartographer_kinematic_limits_tests
    kinematic_limits_tests.cpp
)
target_link_libraries(cartographer_kinematic_limits_tests
    PRIVATE
        carto_kinematic
)
carto_target(cartographer_kinematic_limits_tests)
add_test(NAME cartographer_kinematic_limits_tests
    COMMAND cartographer_kinematic_limits_tests)

add_executable(cartographer_temporal_channel_tests
    temporal_channel_tests.cpp
)
target_link_libraries(cartographer_temporal_channel_tests
    PRIVATE
        carto_temporal
)
carto_target(cartographer_temporal_channel_tests)
add_test(NAME cartographer_temporal_channel_tests
    COMMAND cartographer_temporal_channel_tests)

add_executable(cartographer_motion_sequence_tests
    motion_sequence_tests.cpp
)
target_link_libraries(cartographer_motion_sequence_tests
    PRIVATE
        carto_temporal
)
carto_target(cartographer_motion_sequence_tests)
add_test(NAME cartographer_motion_sequence_tests
    COMMAND cartographer_motion_sequence_tests)

add_executable(cartographer_temporal_sampling_tests
    temporal_sampling_tests.cpp
)
target_link_libraries(cartographer_temporal_sampling_tests
    PRIVATE
        carto_temporal
)
carto_target(cartographer_temporal_sampling_tests)
add_test(NAME cartographer_temporal_sampling_tests
    COMMAND cartographer_temporal_sampling_tests)

add_executable(cartographer_temporal_geometry_sequence_tests
    temporal_geometry_sequence_tests.cpp
)
target_link_libraries(cartographer_temporal_geometry_sequence_tests
    PRIVATE
        carto_temporal
)
carto_target(cartographer_temporal_geometry_sequence_tests)
add_test(NAME cartographer_temporal_geometry_sequence_tests
    COMMAND cartographer_temporal_geometry_sequence_tests)

add_executable(cartographer_archetype_inference_tests
    archetype_inference_tests.cpp
)
target_link_libraries(cartographer_archetype_inference_tests
    PRIVATE
        carto_archetype
)
carto_target(cartographer_archetype_inference_tests)
add_test(NAME cartographer_archetype_inference_tests
    COMMAND cartographer_archetype_inference_tests)

add_executable(cartographer_instance_identity_tests
    instance_identity_tests.cpp
)
target_link_libraries(cartographer_instance_identity_tests
    PRIVATE
        carto_archetype
)
carto_target(cartographer_instance_identity_tests)
add_test(NAME cartographer_instance_identity_tests
    COMMAND cartographer_instance_identity_tests)

add_executable(cartographer_verification_capability_ledger_tests
    verification_capability_ledger_tests.cpp
)
target_link_libraries(cartographer_verification_capability_ledger_tests
    PRIVATE
        carto_verification
        carto_journal
)
carto_target(cartographer_verification_capability_ledger_tests)
add_test(NAME cartographer_verification_capability_ledger_tests
    COMMAND cartographer_verification_capability_ledger_tests)

add_executable(cartographer_verification_donor_promotion_tests
    verification_donor_promotion_tests.cpp
)
target_link_libraries(cartographer_verification_donor_promotion_tests
    PRIVATE
        carto_verification
)
carto_target(cartographer_verification_donor_promotion_tests)
add_test(NAME cartographer_verification_donor_promotion_tests
    COMMAND cartographer_verification_donor_promotion_tests)

add_executable(cartographer_verification_failure_injection_tests
    verification_failure_injection_tests.cpp
)
target_link_libraries(cartographer_verification_failure_injection_tests
    PRIVATE
        carto_verification
        carto_journal
)
carto_target(cartographer_verification_failure_injection_tests)
add_test(NAME cartographer_verification_failure_injection_tests
    COMMAND cartographer_verification_failure_injection_tests)

add_executable(cartographer_verification_regression_gates_tests
    verification_regression_gates_tests.cpp
)
target_link_libraries(cartographer_verification_regression_gates_tests
    PRIVATE
        carto_verification
        carto_journal
)
carto_target(cartographer_verification_regression_gates_tests)
add_test(NAME cartographer_verification_regression_gates_tests
    COMMAND cartographer_verification_regression_gates_tests)

add_executable(cartographer_verification_evidence_report_tests
    verification_evidence_report_tests.cpp
)
target_link_libraries(cartographer_verification_evidence_report_tests
    PRIVATE
        carto_verification
        carto_journal
)
carto_target(cartographer_verification_evidence_report_tests)
add_test(NAME cartographer_verification_evidence_report_tests
    COMMAND cartographer_verification_evidence_report_tests)

add_executable(cartographer_verification_corrupt_artifact_tests
    verification_corrupt_artifact_tests.cpp
)
target_link_libraries(cartographer_verification_corrupt_artifact_tests
    PRIVATE
        carto_verification
        carto_journal
)
carto_target(cartographer_verification_corrupt_artifact_tests)
add_test(NAME cartographer_verification_corrupt_artifact_tests
    COMMAND cartographer_verification_corrupt_artifact_tests)

add_executable(cartographer_community_market_tests
    community_market_tests.cpp
)
target_link_libraries(cartographer_community_market_tests
    PRIVATE
        carto_market
)
carto_target(cartographer_community_market_tests)
add_test(NAME cartographer_community_market_tests
    COMMAND cartographer_community_market_tests)

add_executable(cartographer_source_asset_registry_tests
    source_asset_registry_tests.cpp
)
target_link_libraries(cartographer_source_asset_registry_tests PRIVATE carto_assets)
carto_target(cartographer_source_asset_registry_tests)
add_test(NAME cartographer_source_asset_registry_tests
    COMMAND cartographer_source_asset_registry_tests)

add_executable(cartographer_project_source_asset_registry_tests
    project_source_asset_registry_tests.cpp
)
target_link_libraries(cartographer_project_source_asset_registry_tests
    PRIVATE carto_project carto_journal)
carto_target(cartographer_project_source_asset_registry_tests)
add_test(NAME cartographer_project_source_asset_registry_tests
    COMMAND cartographer_project_source_asset_registry_tests)

add_executable(cartographer_project_authoring_standards_tests
    project_authoring_standards_tests.cpp
)
target_link_libraries(cartographer_project_authoring_standards_tests
    PRIVATE
        carto_application
        carto_ui
)
carto_target(cartographer_project_authoring_standards_tests)
add_test(NAME cartographer_project_authoring_standards_tests
    COMMAND cartographer_project_authoring_standards_tests)

add_executable(cartographer_sqlite_authoring_database_tests
    sqlite_authoring_database_tests.cpp
)
target_link_libraries(cartographer_sqlite_authoring_database_tests
    PRIVATE carto_project)
target_include_directories(cartographer_sqlite_authoring_database_tests
    PRIVATE "${CMAKE_SOURCE_DIR}/libs/project/src")
target_compile_definitions(cartographer_sqlite_authoring_database_tests
    PRIVATE CARTO_SQLITE_TEST_FAULT_INJECTION=1)
carto_target(cartographer_sqlite_authoring_database_tests)
add_test(NAME cartographer_sqlite_authoring_database_tests
    COMMAND cartographer_sqlite_authoring_database_tests
        "$<TARGET_FILE:cartographer_sqlite_authoring_database_tests>")

# Character-production capability family (docuseries Chapters 04-17).
add_executable(cartographer_character_foundation_tests
    character_foundation_tests.cpp
)
target_link_libraries(cartographer_character_foundation_tests PRIVATE carto_character)
carto_target(cartographer_character_foundation_tests)
add_test(NAME cartographer_character_foundation_tests COMMAND cartographer_character_foundation_tests)

add_executable(cartographer_autorig_analysis_tests
    autorig_analysis_tests.cpp
)
target_link_libraries(cartographer_autorig_analysis_tests PRIVATE carto_character)
carto_target(cartographer_autorig_analysis_tests)
add_test(NAME cartographer_autorig_analysis_tests COMMAND cartographer_autorig_analysis_tests)

add_executable(cartographer_autorig_candidate_tests
    autorig_candidate_tests.cpp
)
target_link_libraries(cartographer_autorig_candidate_tests PRIVATE carto_character)
carto_target(cartographer_autorig_candidate_tests)
add_test(NAME cartographer_autorig_candidate_tests COMMAND cartographer_autorig_candidate_tests)

add_executable(cartographer_autorig_constraints_tests
    autorig_constraints_tests.cpp
)
target_link_libraries(cartographer_autorig_constraints_tests PRIVATE carto_character)
carto_target(cartographer_autorig_constraints_tests)
add_test(NAME cartographer_autorig_constraints_tests
    COMMAND cartographer_autorig_constraints_tests)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/character_module_tests.cpp")
    add_executable(cartographer_character_module_tests
        character_module_tests.cpp
    )
    target_link_libraries(cartographer_character_module_tests PRIVATE carto_character)
    carto_target(cartographer_character_module_tests)
    add_test(NAME cartographer_character_module_tests COMMAND cartographer_character_module_tests)
endif()

add_executable(cartographer_rig_module_project_tests
    rig_module_project_tests.cpp
)
target_link_libraries(cartographer_rig_module_project_tests
    PRIVATE
        carto_character
        carto_journal
        carto_project
)
carto_target(cartographer_rig_module_project_tests)
add_test(NAME cartographer_rig_module_project_tests
    COMMAND cartographer_rig_module_project_tests)

add_executable(cartographer_character_records_project_tests
    character_records_project_tests.cpp
)
target_link_libraries(cartographer_character_records_project_tests
    PRIVATE
        carto_journal
        carto_motion
        carto_pose
        carto_project
        carto_weights
)
carto_target(cartographer_character_records_project_tests)
add_test(NAME cartographer_character_records_project_tests
    COMMAND cartographer_character_records_project_tests)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/workspace_tests.cpp")
    add_executable(cartographer_workspace_tests
        workspace_tests.cpp
    )
    target_link_libraries(cartographer_workspace_tests
        PRIVATE
            carto_workspace
            carto_journal
    )
    carto_target(cartographer_workspace_tests)
    add_test(NAME cartographer_workspace_tests COMMAND cartographer_workspace_tests)
endif()

add_executable(cartographer_character_production_tests
    character_production_tests.cpp
)
target_link_libraries(cartographer_character_production_tests PRIVATE carto_production carto_kinematic)
carto_target(cartographer_character_production_tests)
add_test(NAME cartographer_character_production_tests COMMAND cartographer_character_production_tests)

add_executable(cartographer_weights_tests
    weights_tests.cpp
)
target_link_libraries(cartographer_weights_tests PRIVATE carto_weights)
carto_target(cartographer_weights_tests)
add_test(NAME cartographer_weights_tests COMMAND cartographer_weights_tests)

add_executable(cartographer_pose_tests
    pose_tests.cpp
)
target_link_libraries(cartographer_pose_tests PRIVATE carto_pose)
carto_target(cartographer_pose_tests)
add_test(NAME cartographer_pose_tests COMMAND cartographer_pose_tests)

add_executable(cartographer_deform_tests
    deform_tests.cpp
)
target_link_libraries(cartographer_deform_tests PRIVATE carto_deform)
carto_target(cartographer_deform_tests)
add_test(NAME cartographer_deform_tests COMMAND cartographer_deform_tests)

add_executable(cartographer_face_tests
    face_tests.cpp
)
target_link_libraries(cartographer_face_tests PRIVATE carto_face)
carto_target(cartographer_face_tests)
add_test(NAME cartographer_face_tests COMMAND cartographer_face_tests)

add_executable(cartographer_motion_tests
    motion_tests.cpp
)
target_link_libraries(cartographer_motion_tests PRIVATE carto_motion)
carto_target(cartographer_motion_tests)
add_test(NAME cartographer_motion_tests COMMAND cartographer_motion_tests)

add_executable(cartographer_sim_tests
    sim_tests.cpp
)
target_link_libraries(cartographer_sim_tests PRIVATE carto_sim)
carto_target(cartographer_sim_tests)
add_test(NAME cartographer_sim_tests COMMAND cartographer_sim_tests)

add_executable(cartographer_gpudef_tests
    gpudef_tests.cpp
)
target_link_libraries(cartographer_gpudef_tests PRIVATE carto_gpudef)
carto_target(cartographer_gpudef_tests)
add_test(NAME cartographer_gpudef_tests COMMAND cartographer_gpudef_tests)

# Keep test-owned temporary files inside the writable build tree. The default
# Windows temp directory is outside the workspace sandbox in some developer
# environments, which turns otherwise valid atomic-save/publication tests into
# permission failures. Separate directories also prevent parallel tests from
# colliding through fixed temporary filenames.
get_property(CARTO_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
set(CARTO_TEST_TEMP_INDEX 0)
foreach(CARTO_TEST_NAME IN LISTS CARTO_REGISTERED_TESTS)
    math(EXPR CARTO_TEST_TEMP_INDEX "${CARTO_TEST_TEMP_INDEX} + 1")
    set(CARTO_TEST_TEMP_DIR "${CMAKE_SOURCE_DIR}/.ctest-tmp/${CARTO_TEST_TEMP_INDEX}")
    file(MAKE_DIRECTORY "${CARTO_TEST_TEMP_DIR}")
    set_tests_properties("${CARTO_TEST_NAME}" PROPERTIES
        ENVIRONMENT
            "TMP=${CARTO_TEST_TEMP_DIR};TEMP=${CARTO_TEST_TEMP_DIR};TMPDIR=${CARTO_TEST_TEMP_DIR}")
endforeach()
