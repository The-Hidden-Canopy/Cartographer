# Temporal motion-vector sign contract

## Repair ledger

- **Files changed:** `libs/d3d12/shaders/cartographer_temporal.hlsl`,
  `libs/d3d12/shaders/cartographer_temporal_fp8.hlsl`,
  `tests/d3d12_scene_renderer_tests.cpp`, `tests/d3d12_tests.cpp`, this ledger,
  and the ignored local work queue.
- **Capability gained:** positive current-to-history motion offsets now sample
  the pixel at `current + offset`, matching the documented top-left pixel-space
  contract: `+X` selects the right pixel and `+Y` selects the row below.
- **Boundary improved:** the renderer no longer reverses a valid reprojection
  vector and samples the opposite side of the frame. The authored GPU
  regression uploads a distinguishable history image and positive one-pixel
  X/Y fields, then checks right-neighbor and next-row sampling plus off-frame
  current-pixel fallback. The FP8 benchmark variant now shares the
  same offset convention, rejects nonfinite or over-extent flow, and falls back
  to current pixels for off-frame reprojection instead of clamping to an edge.
  Its GPU fixture includes NaN and over-extent vectors to exercise both
  fail-closed paths. FP8 motion bytes are loaded once per pixel.
- **Governance preserved:** the change is confined to read-only rendering and
  caller-owned temporal resources. Request provenance, resource-state
  transitions, project authority, persisted schema, and promotion boundaries
  are unchanged.
- **Tests added or updated:** extended the D3D12 temporal integration source
  with positive-X and positive-Y sampling cases; updated the FP8 GPU source fixture for
  the shared sign convention, off-frame fallback, nonfinite-motion refusal,
  and excessive-motion refusal.
  **Run:** none, per the user's instruction.
- **Tests not run:** D3D12 builds, HLSL compilation, both D3D12 test suites,
  GPU execution, desktop motion production, and visual review.
- **Risks:** the shader and regression source are uncompiled. The synthetic
  test flow validates address direction, not the quality of a production
  motion producer; the desktop still has no camera-reprojection producer or
  persistent temporal history owner.
- **Rollback:** restore subtraction/clamping in both temporal shader variants
  and restore the prior FP8 fixture and this ledger entry. No saved project or
  cache migration is required.
- **Rejected:** changing the documented API convention to match the reversed
  shader, adding an implicit sign flip in callers, or claiming desktop
  accumulation is wired.

## Cross-product and promotion trace

Direct impact is Cartographer's D3D12 temporal resolve shader. RegOS,
IDA_Training, hidden_canopy_bot, and Ask_IDA have no direct or indirect code,
API, schema, dataset, evaluation, or deployment changes. Dataset and training
progress are unchanged; capability progress is source-only; promotion remains
open pending shader compilation, GPU execution, producer wiring, and native
acceptance.
