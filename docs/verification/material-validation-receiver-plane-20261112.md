# Isolated material validation and receiver-plane shadow correction

Date: 2026-11-12. Candidate built from `342fdd184f5c` plus the local validation fixes recorded here. This is independent evidence, not validation of current main.

## Build and executed checks

Fresh local build directory: `build-material-validation-20261112`. Clang 22.1.8 (LLVM-MinGW), Ninja, Debug, D3D12 enabled, tests enabled, CLI and desktop targets disabled. Built `cartographer_authoring_material_tests` and `cartographer_d3d12_scene_renderer_tests` serially with `--parallel 1`. Both compiled and linked. Existing shadow/aggregate-initializer warnings remain; warnings are not errors.

Final focused command:

```
ctest --test-dir build-material-validation-20261112 -R "^(cartographer_authoring_material_tests|cartographer_d3d12_material_.*_tests)$" --output-on-failure --parallel 1
```

Result: **5/5 passed, 52.88 seconds**.

- authoring material: 0.55 s; includes exact six-map two-pass bake parity and explicit UV-domain cases, existing material/lifecycle cases.
- graph opacity CPU admission: 6.81 s.
- graph sheen and exact-neutral physical output CPU admission: 11.24 s.
- anisotropy/frame/normal-map CPU admission: 8.21 s.
- focused native GPU material checks: 25.98 s. Occlusion, opacity, sheen, signed anisotropy, normal-mapped anisotropy, cache-versus-fresh parity, point/spotlights including an occluded receiver, and transformed normal/tangent parity passed. Debug receipt list remained empty and device was not lost.

The focused GPU run also passed directly before its CTest rerun. No interactive window, subjective visual acceptance, full current-main suite, target-engine import or performance benchmark is claimed.

## Real shader defect found and corrected

The directional sheen fixture produced 0.028934 rather than the unshadowed reference 0.034401. Its direct-light component was exactly two-thirds of the expected result: the old 3x3 PCF comparison used one center receiver depth for every neighboring shadow texel, falsely self-shadowing a sloped receiver.

The shader now computes the receiver depth gradient in shadow UV outside light/visibility branches and compares each PCF sample at its own receiver-plane depth. Nonfinite or ill-conditioned derivatives fall back to zero correction. Existing bias, bounds, and depth-map sampling remain. Strict BRDF pixel tolerances were not relaxed. The existing spotlight occluder check confirms real occlusion still attenuates its receiver.

## Regression fixture corrections

- Secondary UV fixture no longer declares two active render UV sets.
- Opaque coverage compares RGB and depth; its authored HDR alpha payload may differ without enabling blending.
- Point-light iterations clear spot-only cone angles; spot iterations restore their angles.
- Diagnostic failure includes actual/expected sheen values.
- Four focused CTest modes are registered without replacing the full scene test.

## Full-suite limitation

The full scene executable was attempted and failed before GPU execution at equirectangular environment derivation: `prefiltered environment radiance is non-finite or exceeds its bound`. This was not suppressed or counted as passed. Focused modes expose independent material evidence while that aggregate gate remains unresolved in this isolated base.

## Comparison with main and integration scope

Pinned remote `4b9944651d3b151ed9a30aa919770345dd7506ba` includes all original opacity/sheen/anisotropy and bake/domain implementations, and reports 228/228 integrated tests. It still contains the old center-depth PCF and the secondary-UV, opaque-alpha, and point-cone fixture issues corrected here. Its full test entry calls those cases. Reconcile the exact source/build/command behind that reported result before treating it as independently reproduced by this run.

Main also added `program.executable()` rejection in the bake entry. Therefore **do not port the preceding local-only default-program compatibility test commit**: main's original refusal expectation is correct under its newer admission. The shader/scene-test/CMake changes in this receipt are the unique integration handoff.
