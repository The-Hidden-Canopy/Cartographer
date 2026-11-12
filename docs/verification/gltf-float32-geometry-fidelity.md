# glTF and GLB float32 geometry fidelity

## Code-only continuation (2026-11-12)

The exporter already refused triangles that collapse when authoring positions
are narrowed to float32. Its area-only test could nevertheless admit a valid,
noncollapsed triangle whose winding reverses during rounding. It also computed
cross products and squared area in float, rejecting large but finite float32
positions solely because the validation arithmetic overflowed.

The candidate checks the actual packed float32 positions using double-precision
edge, cross-product, and squared-area arithmetic, matching native PBR upload's
existing geometry policy. The minimum squared cross-product magnitude remains
`4e-24`. It additionally requires a positive dot product between the source and
packed geometric area vectors. Reversed or orthogonal area directions refuse
before file publication. Source winding, normals, and node transforms are not
rewritten to disguise loss. Authored clockwise geometry and mirrored node scale
remain supported when narrowing preserves the source geometry.

The check is independent of UV presence and material shading. It does not
change material admission, tangent generation, project serialization, the
render-artifact version, or the glTF/GLB structure. There are no new packages,
private kernels, external fixtures, or source mutations.

## Regression source

`gltf_float32_position_projection_preserves_fidelity` is registered in
`cartographer_gltf_geometry_parity_tests`. It contains:

- a rounding-reversed triangle in each of the XY, YZ, and ZX planes, with and
  without UVs and under both positive and mirrored node scale;
- exact refusal diagnostics for both glTF and GLB, unchanged existing files,
  unchanged source serialization, and no new partial publication after reopen;
- ordinary counterclockwise, clockwise, tilted, large squared-area, and large
  cross-product cases, with both UV states and node scale signs;
- exact packed position values and source winding, glTF/GLB payload parity,
  reopen determinism, and a successful export after prior refusal.

The existing float32-collapse regressions remain. All cases are authored
expectations, not observed behavior.

## Validation and limits

Only manual source review, patch application checks, and whitespace checks are
performed for this candidate. No build, compiler, test executable, benchmark,
GPU run, or consumer import is performed while the build hold remains active.
Run the geometry-parity target, production-closure target, and normal headless
gates on the approved combined integration tree, then check actual downstream
import and native visual results.

The policy compares the geometry admitted by the existing double-precision
source artifact. It is not an exact-arithmetic orientation predicate or a
position-error/shape-error bound, and does not prove end-to-end visual fidelity.
Downstream world-size, transform, renderer, and precision limits still apply.

This is patch 4 after mirrored-UV tangent domains, native PBR float32 fidelity,
and glTF float32 UV fidelity. It was prepared in a separate file overlay so the
shared staging tree and integration owner's work remain untouched. Material
and sculpt implementation lanes are outside this patch.
