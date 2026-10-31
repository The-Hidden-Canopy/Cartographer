# Cartographer–VANTA integration, 2026-10-31

Status: started. Cartographer base `ceb8900`; VANTA base
`ab71deae8b497ab48c6586b8f6dd4d6070998605`. The local canonical work queue
records the integration owner and isolated branches. This work owns the shared
asset/editor/runtime boundary, including prepared-scene and glTF fidelity,
explicit unsupported diagnostics and their real callers. Existing worktree and
branch changes are not replaced.

CPU acceptance under this claim ran on 2026-10-31 at Cartographer `82b67a2`:
`cmake --preset default`, `cmake --build build/default --target
cartographer_gltf_geometry_parity_tests`, and `ctest --test-dir build/default
-R cartographer_gltf_geometry_parity_tests --output-on-failure` passed, along
with the aggregate export suites (`cartographer_tests`,
`cartographer_uv_material_contract_tests`,
`cartographer_generated_gltf_import_tests`,
`cartographer_provider_plugin_tests`,
`cartographer_gltf_glb_cli_export_tests`). These are headless CPU checks of
authored geometry preservation, explicit loss refusal, and glTF/GLB payload
parity. They are not evidence of appearance preservation, GPU or native
visual acceptance, successful Studio reimport, or release readiness.

No fetch, push, publication or deployment is authorized by this record.
