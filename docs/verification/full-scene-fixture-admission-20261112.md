# Full-scene fixture admission corrections (2026-11-12)

Isolated material candidate, following f6e3055 and 83d4dbc. No production source
or shared integration checkout changed in this continuation.

Corrected invalid test inputs:
- Perspective and orthographic camera records now supply required bookmark IDs.
- Quaternion-derived camera target comparisons allow 1e-12 numerical error;
  translation, authored projection, clip range and exposure comparisons remain exact.
- Secondary directional light uses the required zero range.
- Vertex and face color fixtures configure domain counts before adding attributes.
- Extra material UV sets are not all marked the one active render UV set.

Added camera-only and colors-only diagnostic entry points. Both passed in the
isolated Debug LLVM-MinGW Clang 22.1.8 build. All existing runtime validation is
preserved. The default full scene entry is neither skipped nor weakened.

The five prior material/authoring focused CTests passed again during this run,
including native material GPU checks. That run's full scene case exposed the
subsequently corrected directional fixture. Later full-scene reruns progressed
through camera, light and color checks before exposing the multi-UV fixture.

Final full-scene result is recorded below after the last run. This is not a
repository-wide 228-test build or independent acceptance on main. Only the
material and complete scene-renderer test executables were built here.

Final result: complete `cartographer_d3d12_scene_renderer_tests` CTest PASSED,
114.87 seconds (114.99 seconds total), exit 0. This executes the default entry,
including all source checks and native GPU paths, not a diagnostic subset.
The capture receipt assertion also now expects the actual four production
limitations and explicitly requires the transparency warning; shader-digest and
hardware checks are preserved. No production code was changed by this commit.

## Integrated on main (2026-11-12)

The receiver-plane PCF, environment blue-channel and fixture corrections were
applied to main after `4b99446`. Integration also:

- bumped the scene capture algorithm to `scene-direct-capture.v35`, because PCF
  and LDR environment decoding change rendered pixels;
- updated the scene-render CLI check, which still expected `v31`, and compared
  its decimal receipt fields numerically: the receipt writes 17 significant
  digits, so `0.9` is spelled `0.90000000000000002`;
- gave the CLI authored-camera fixture the bookmark IDs that admission requires.

Release build (LLVM-MinGW Clang, D3D12 on): authoring material, full D3D12
scene renderer, the four focused material GPU gates and the scene-render CLI
test passed, 7/7. The CPU-only suite passed 228/228.

The full D3D12-enabled suite first passed 248/250. The two failures predated
this run and are now fixed:

- D3D12 bent-normal bakes never succeeded. The report compared the projection
  worker's settings digest, which is the recipe digest, with the bent-normal
  artifact's domain-separated settings digest. It now binds the worker to the
  recipe digest; artifact validation still binds the artifact's own digest.
- `cartographer_d3d12_tests` targeted the realism material contract, but
  `cartographer_pbr.hlsl` still held the earlier shader with an ambient
  surrogate and in-shader exposure. The file is now the define-free path of
  `cartographer_authoring_pbr.hlsl`. The same suite also expected a 50% temporal
  blend that the 3x3 neighborhood clamp forbids for a uniform frame, and
  cleared a motion target to infinity, which device commands refuse. Both
  expectations now match the shipped contracts.

After these fixes the D3D12-enabled suite passed 250/250 on the NVIDIA Quadro
P5200.
