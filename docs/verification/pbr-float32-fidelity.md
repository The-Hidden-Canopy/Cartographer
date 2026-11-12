# Native PBR float32 fidelity boundary

## Code-only continuation (2026-11-12)

The native PBR upload projection no longer accepts finite values alone as proof
that double-precision authoring geometry and UVs survive float32 conversion.
`PbrMeshUpload::validate` rejects repeated indices, collinear/collapsed
triangles, and triangles below the existing render-artifact area bound. Its
area arithmetic uses double precision over the actual float32 coordinates so
large representable triangles do not overflow a float32 cross product.

`pack_pbr_mesh_upload` additionally compares source and converted triangles.
It refuses a reversed surface orientation and any change in primary-UV
orientation, including loss of nonzero UV area. A UV orientation reversal can
occur even when all three converted corners remain distinct. These checks
apply to both the strict and explicit texture-free overloads.

Representable mirrored UVs remain supported. UV orientation uses a sign test,
with power-of-two-scaled products rather than an absolute area threshold, so
tiny but nonzero representable UV charts
remain admissible. A conservative rounding bound and subtraction residuals
refuse unresolved near-collinear orientations instead of treating uncertain
zero area as proof. Authored constant UVs remain allowed when conversion keeps
them constant. Missing UVs remain neutral only under the existing validated
texture-free-material proof. No source mutation or alternate fallback occurs.

## Regression cases authored

`cartographer_production_closure_tests` includes:

- direct upload rejection of repeated indices and distinct collinear vertices;
- double-valid geometry and UV triangles that collapse only after conversion;
- geometry and UV sign reversals caused by rounding without merged corners,
  plus refusal of rounding that invents area in a collinear UV chart;
- exact input-artifact preservation after refusal;
- representable large geometry and tiny UV coordinates, with explicit refusal
  of tiny/anisotropic authored UVs whose unscaled determinant underflows but
  whose nonzero orientation is lost in float32, including cancellation during
  edge subtraction;
- preserved mirrored and constant UVs; and
- successful texture-free neutral UVs, with no bypass for collapsed geometry.

## Compatibility and verification

The 52-byte upload ABI and render-artifact v4 remain unchanged. No project or
artifact migration is needed. Some previously accepted lossy uploads now fail
with an explicit diagnostic. Existing source/mesh identities and material
proof requirements are unchanged.

This is code-only work. `git diff --check` passed. Independent static review
identified two numerical edge cases, now covered by source changes and
regression fixtures, and found no further concrete issue on the final review.
The regression cases were added but not executed. Compilation, the focused production-closure tests, full
headless suite, D3D12 upload checks, and visual acceptance remain pending.
This is not a bound on every float32 rounding error, a rebase policy, or a
claim that downstream world transforms and every renderer/exporter use these
checks. The scope is the shared native PBR mesh upload projection.
