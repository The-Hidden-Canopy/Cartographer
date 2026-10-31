# D3D12 viewport pan (2026-10-26)

## Capability delta

The standalone D3D12 project viewport now has middle-button pan alongside
left-button orbit. Pan uses logical pointer deltas, then converts them to
camera-plane world movement from the fitted camera distance, vertical field of
view, current viewport height, and current monitor DPI. Each dirty frame now
rebuilds the camera from an immutable base snapshot. Repeated temporal samples
therefore do not apply the absolute pan offset to the preceding sample's target,
and Home restores the original fitted target.

## Repair ledger

- **Files changed:** `libs/d3d12/include/carto/d3d12/scene_camera_navigation.hpp`,
  `apps/d3d12_desktop/main.cpp`, `tests/d3d12_scene_renderer_tests.cpp`, this
  ledger, and the ignored local work queue.
- **Capability gained:** pan, orbit, zoom, and Home now derive from one stable
  camera base; repeated temporal samples do not accumulate camera translation.
  Orthographic zoom adjusts its projection height.
- **Invariants preserved:** camera navigation remains transient and read-only;
  no project camera, schema, journal, domain event, authority, or saved source
  state changes.
- **Tests authored:** repeated-frame pan determinism, Home target restoration,
  and orthographic zoom behavior in `cartographer_d3d12_scene_renderer_tests`.
  **Run:** none, at the user's instruction.
- **Tests not run:** no build, platform-neutral input suite, D3D12 execution,
  or native interaction.
- **Risks:** the new header and application call path are source-only; runtime
  camera framing, temporal interaction, and DPI/resize behavior remain
  unverified.
- **Rollback:** remove the helper and its desktop/test call sites, restore the
  prior camera update function, and revert this ledger/queue continuation.
- **Rejected:** persisting viewport pan into authored project camera state or
  accumulating camera offsets from each rendered request.

## Governance preserved

- State transition path: pan only changes transient camera presentation state;
  it does not modify project data.
- Audit/domain event path: no domain mutation or project receipt was added.
- Permission/org-scope path: no authority or organization scope was added.
- Human approval boundary: the saved scene remains read-only.

## Tests

- Added: repeated-frame pan stability and Home restoration; orthographic zoom
  source regressions.
- Updated: the D3D12 scene-renderer regression source.
- Run: none, at the user's request to code without testing.
- Not run: platform-neutral input tests, D3D12 desktop build, native pan,
  resize, and DPI interaction.

## Rejected options

Did not encode pan into the saved project camera or change orbit, wheel zoom,
keyboard nudges, or the existing single-pointer capture policy.

## Risks and rollback

Pan math, immutable base-camera integration, and Win32 routing are
source-reviewed only; sign conventions, camera framing across DPI/resize
changes, and native capture behavior remain unverified. Roll back the new
`scene_camera_navigation.hpp` helper and its shell/test wiring to restore the
previous camera update path; retain the earlier orbit/pan input work.

## Cross-product trace

- Direct impact: optional Cartographer D3D12 desktop camera navigation.
- Indirect impact: none established for RegOS, IDA_Training,
  hidden_canopy_bot, or Ask_IDA.
- Shared contract touched: transient `ViewportInputState` and the existing
  D3D12 scene camera request.
- API/schema risk: no serialized schema or project format changed.
- Dataset/eval risk: none; no evaluation data changed.
- Deployment risk: D3D12 desktop was not rebuilt; native pointer/DPI routing
  remains unverified.
- Required follow-up tests: bounded pan state and Home reset; equal logical
  pan across DPI scales; orbit/pan capture arbitration; source camera-target
  projection at portrait/landscape sizes; native middle-button and DPI/resize
  interaction.

## Promotion status

- Dataset progress: unchanged.
- Training progress: unchanged.
- Capability progress: source input and camera wiring added; runtime behavior
  unverified.
- Promotion progress: unchanged; no promotion claimed.
