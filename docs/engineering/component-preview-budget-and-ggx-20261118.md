# Detailed component scene admission and smooth GGX precision

Two detailed, independently reusable ears total 120,640 triangles. The saved native scene validates, but the authoring GPU preview rejected it at the old 65,536-triangle aggregate limit. A matching CPU/GPU regression also failed that limit. The shared authoring scene limit is now 262,144 visible-instance triangles, so both components can render together without reducing their geometry or texture detail.

The existing GPU viewport suite separately exposed a smooth-highlight precision failure: pixel 2015 had reference HDR 78.836691 versus GPU 78.942413. The unchanged test source from parent `8691357b599594b5ac0c3533d69f7d1f572e7178` reproduces the same failure. The authoring GGX shader now calculates sin-squared from the cross product of normalized normal/half vectors, avoiding cancellation from subtracting a rounded cosine-squared from one. The GGX model, roughness floor and test tolerances are unchanged. A clamped back-hemisphere branch retains the old finite behavior for opposed vectors.

## Files and capability

- `libs/production/include/carto/production/authoring_viewport.hpp`: one shared bounded scene-triangle constant.
- `libs/production/src/authoring_viewport.cpp` and `libs/d3d12/src/authoring_viewport.cpp`: use the same aggregate limit.
- `libs/d3d12/shaders/cartographer_authoring_pbr.hlsl`: numerically stable base/clearcoat GGX distribution.
- `tests/authoring_triangle_budget_fixture.hpp`: reusable governed fixture with 262,144 triangles across bounded mesh instances, plus an exactly-one-triangle overflow.
- `tests/authoring_viewport_budget_tests.cpp` and `tests/authoring_gpu_viewport_tests.cpp`: successful rendering at the boundary, overflow rejection, unchanged sources and recovery; GPU recovery preserves the target and warm geometry. The GPU suite also compares an opposed authored normal against the reference.

Detailed components can now coexist in CPU and GPU authoring previews. Smooth highlights meet the original reference tolerance after the existing suite exposed a precision defect.

## Invariants and verification

Per-mesh bounds, per-draw primitive identity packing, the 64 MiB geometry preparation/cache ceiling, 256 MiB material residency, 320 MiB material staging, texture/program counts and pixel/history limits remain independent admission gates. No project schema, serialized state, user permissions, import rights, journal path or live deployment changes.

Five unique targeted CTests pass: authoring viewport, viewport budget, GPU viewport, temporal reference and shader compilation. After adding the explicit opposed-normal case, the GPU viewport and shader compilation tests pass again with unchanged tolerances. The actual saved 120,640-triangle ear pair renders three native 2048-square views with zero error-magenta pixels. The same saved eye assembly, light and camera rendered by the sealed d61-era and updated executables differs by at most one RGB byte in only 5/25/3 pixels across its three 512-square reviews; approved optical source files remain unchanged. These checks establish specific admission, numerical and appearance-preservation properties, not photorealistic art acceptance.

Not run: unrelated RegOS/IDA/bot/Ask_IDA/backend/frontend suites, a full application/release build, full-wolf integration, animation or alternate-consumer visual acceptance. No running application was replaced or restarted.

## Risks, rejected options and rollback

Worst-case admitted triangle work increases fourfold; CPU preparation/raster time can rise. Independent memory and descriptor limits may still reject a larger scene. The shared header requires consumers to rebuild. Float32 GPU arithmetic still has bounded reference tolerance.

Rejected: lowering component quality to fit the old limit, removing the limit, changing per-draw identity packing or material-memory limits, loosening HDR tolerances, replacing approved eye assets, consuming uncommitted peer work, restarting/deploying the application, or equating technical success with photorealistic quality.

Rollback: revert the commit containing this report to restore the prior admission and authoring shader behavior while preserving parent vertex-color/cache integration. Reverting the corresponding Condition pin commit separately restores its prior dependency revision. No migration or asset deletion is required.

Direct impact: Cartographer authoring preview; Condition receives a separately recorded local source pin. Shared contracts: bounded native rendering and unchanged native/glTF project formats. RegOS, IDA_Training, hidden_canopy_bot and Ask_IDA have no state, org-scope, role, API/schema, dataset/eval or deployment change. Native authoring fixtures continue to use journaled transactions; no backend mutation authority is added.

Dataset progress: none. Training progress: none. Capability progress: verified component coexistence and tighter numerical agreement. Promotion progress: local source validation/pinning only; full art and runtime acceptance remain unfinished.
