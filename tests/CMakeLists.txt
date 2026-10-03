add_executable(cartographer_tests
    test_main.cpp
)

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
            carto_device
            carto_device_ir
            carto_gpu
    )
    target_compile_definitions(cartographer_d3d12_tests
        PRIVATE
            CARTO_D3D12_SHADER_DIR="${CMAKE_SOURCE_DIR}/libs/d3d12/shaders"
    )
    if(WIN32)
        target_link_libraries(cartographer_d3d12_tests PRIVATE user32)
    endif()
    carto_target(cartographer_d3d12_tests)
    add_test(NAME cartographer_d3d12_tests COMMAND cartographer_d3d12_tests)
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
        ${CMAKE_SOURCE_DIR}/libs/plugin_package/src
)
carto_target(cartographer_plugin_package_tests)
add_test(NAME cartographer_plugin_package_tests COMMAND cartographer_plugin_package_tests)

add_executable(cartographer_ui_tests
    ui_tests.cpp
)

add_executable(cartographer_public_api_tests
    public_api_tests.cpp
)

if(NOT CARTO_BUILD_PRIVATE_INTEGRATIONS)
    add_executable(cartographer_public_boundary_tests
        public_boundary_tests.cpp
    )
    carto_target(cartographer_public_boundary_tests)
    add_test(NAME cartographer_public_boundary_tests COMMAND cartographer_public_boundary_tests)
endif()

add_executable(cartographer_preview_transaction_tests
    preview_transaction_tests.cpp
)

add_executable(cartographer_ai_tests
    ai_tests.cpp
)

add_executable(cartographer_topology_edit_tests
    topology_edit_tests.cpp
)

if(CARTO_BUILD_PRIVATE_INTEGRATIONS)
    add_executable(cartographer_owh_model_tests
        "${CARTO_PRIVATE_SOURCE_ROOT}/libs/owh/tests/model_tests.cpp"
    )
    target_link_libraries(cartographer_owh_model_tests PRIVATE carto_owh)
    carto_target(cartographer_owh_model_tests)
    add_test(NAME cartographer_owh_model_tests COMMAND cartographer_owh_model_tests)

    add_executable(cartographer_anatomy_catalog_tests
        "${CARTO_PRIVATE_SOURCE_ROOT}/tests/anatomy_catalog_tests.cpp"
    )
    target_link_libraries(cartographer_anatomy_catalog_tests PRIVATE carto_anatomy)
    carto_target(cartographer_anatomy_catalog_tests)
    add_test(NAME cartographer_anatomy_catalog_tests COMMAND cartographer_anatomy_catalog_tests)
endif()

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
add_test(NAME cartographer_provider_plugin_tests COMMAND cartographer_provider_plugin_tests)

target_link_libraries(cartographer_ui_tests
    PRIVATE
        carto_application
        carto_ui
)

carto_target(cartographer_ui_tests)
add_test(NAME cartographer_ui_tests COMMAND cartographer_ui_tests)

target_link_libraries(cartographer_public_api_tests
    PRIVATE
        carto_application
        carto_editor
        carto_mesh_attributes
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
)

carto_target(cartographer_topology_edit_tests)
add_test(NAME cartographer_topology_edit_tests COMMAND cartographer_topology_edit_tests)
