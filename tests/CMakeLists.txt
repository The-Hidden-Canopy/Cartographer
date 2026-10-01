add_executable(cartographer_tests
    test_main.cpp
)

add_executable(cartographer_application_tests
    application_tests.cpp
)

add_executable(cartographer_render_architecture_tests
    render_architecture_tests.cpp
)

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
