# glTF and GLB float32 UV fidelity

## Code-only continuation (2026-11-12)

The interchange exporter now applies the same reviewed orientation certificate
used by native PBR upload to the actual float32 `TEXCOORD_0` coordinates of each
emitted primitive. Finiteness alone no longer permits a chart to collapse,
reverse, gain area from a collinear source, or pass with an uncertain source
orientation after narrowing.

The check runs independently of tangent generation. Valid authored frames or
a supported UV-only fallback cannot hide UV loss. Only triangles referenced by
the emitted primitive are checked; unrelated faces in another material's UV
stream do not become a new requirement. Export fails before publication, with
an explicit float32 UV diagnostic, without replacing an existing file or
mutating the source project.

The shared public `validate_uv_triangle_projection` helper validates finite
coordinates and edge differences before using the existing power-of-two
product scaling, compensated determinant, and conservative subtraction-error
certificate. Native PBR now calls the same helper, preserving its earlier
orientation gate. No private kernel, external dependency, or format revision
is introduced.

## Regression cases authored

`cartographer_gltf_geometry_parity_tests` now includes both glTF and GLB cases
for collapsed, reversed, underflowed, anisotropic, subtraction-loss, and
invented-area UV triangles. Each case runs with and without authored tangent
frames and checks unchanged prior files and complete source serialization.
Failure at a new destination must leave no published artifact.

Positive cases check exact float32 accessor values and glTF/GLB payload parity
for ordinary, mirrored, tiny representable, and constant UVs. The shared
helper also has direct non-finite-coordinate and overflowing-edge cases.

## Persistence and acceptance boundary

Project serialization, render-artifact v4, and glTF/GLB structure are unchanged.
Previously accepted lossy or uncertain UV projections now refuse. This check
does not bound every coordinate-rounding error, replace tangent generation,
change the material pipeline, or claim a complete interchange-fidelity result.

Manual static review and `git diff --check` completed. The new regression cases
were authored but not run. Build, the geometry-parity and production-closure
targets, full headless tests, consumer import, and visual acceptance remain
pending. This patch follows the mirrored-UV tangent and native PBR float32
fidelity patches in that order.
