# Masked fur mip filtering, 2026-11-16

A synthetic red fiber over invisible white texels reproduced a rendering
defect: ordinary mip filtering dropped every texel below the material's mask
cutoff and mixed the hidden matte color into the remaining fiber. The initial
regression failed with `minified masked fur lost all coverage` before the
production repair. The ear studies also exposed a separate authoring problem:
their underlying material carried photographed shading. That is being replaced
with a native geometry-baked coat; this filter repair does not establish final
art quality.

## Repair ledger

- Files changed: `libs/assets/include/carto/assets/texture_mip.hpp`,
  `libs/assets/src/texture_mip.cpp`,
  `libs/production/include/carto/production/authoring_material.hpp`,
  `libs/production/src/authoring_material_surface.cpp`,
  `tests/authoring_surface_tests.cpp`, `tests/CMakeLists.txt`, and this ledger.
- Capability gained: direct masked base-color textures retain minified fiber
  coverage and filter color from accepted fibers. Invisible matte colors no
  longer bleed into those mip levels. Level-zero pixels and source bytes are
  retained exactly.
- Boundary improved: the cache distinguishes opaque, masked and different
  effective cutoff derivations. Material alpha factors are included in that
  cutoff. A wholly rejected material gains no invented visible coverage.
- Preserved: ordinary opaque/data/normal mip generation, work budgets, immutable
  source/program snapshots, authored catalogs, graph-bound alpha behavior,
  source digests/revisions, state transitions and journal paths. No permission,
  org-scope or human-approval authority changes.
- Tests: source texture, authoring material, texture snapshot, two-sided
  CPU/GPU shading, preview return/layout and preview sampling suites passed.
  The affected authoring-surface suite passed after its native matte-color
  fixture explicitly set F0 to zero, separating matte color from a white
  specular lobe. Its CPU and D3D12 checks exercise minification, shading,
  opaque -> masked -> opaque cache reuse, cutoff/factor differences, fully
  transparent inputs, graph boundaries, source immutability, artifact
  serialization and invalid normal-map/zero-cutoff requests.
- Not run: full repository suite and graphical Condition gameplay. Verification
  covers the changed derived texture/material contract, not a complete fur
  shader, all lighting conditions, anatomy, animation or production art approval.
- Risks: changed minification appearance for direct masked color maps. The
  new derived algorithm identity, `carto.rgba8-mip.masked-color-box.v1`, is
  explicit; older readers reject it. Project serialization is unchanged.
- Rollback: revert this commit and rebuild static consumers. Saved PNGs,
  material source and projects need no migration. Derived mip caches can be
  regenerated under the previous algorithm.
- Rejected: modifying source alpha/RGB bytes, forcing every graph into a
  guessed alpha predicate, widening all fibers, raising budgets, disabling
  mipmapping or treating a green test as photorealistic art acceptance.

## Contract and impact trace

`AuthoringTextureLibrary::prepare` accepts an optional nonzero effective
8-bit masked-color cutoff. Existing source callers keep the default behavior;
static consumers must rebuild. The asset mip options gain an opt-in masked
color filter. Its first reduction classifies the source mask, excludes rejected
RGB, and preserves coverage; later reductions filter color with alpha weights.
The source level stays unchanged. Only direct material bindings with a known
constant alpha factor opt in. Graph-bound alpha, zero cutoffs and fully rejected
materials retain their previous path.

Direct impact is Cartographer derived authoring mips and their CPU/native GPU
consumers. Condition must consume the rebuilt Cartographer pin. No RegOS,
IDA_Training, hidden_canopy_bot or Ask_IDA source, API or dataset changes. No
database migration or deployment script. The ordinary mip algorithm remains
available and unchanged; there is a new recognized derived-artifact algorithm,
not a new project schema. Existing broader GPU parity tolerance failure remains
outside this repair and unresolved. Interactive canvas reuse, final fur
appearance and game performance require their own evidence.

Dataset progress: none. Training progress: none. Capability progress: tested
masked-fiber minification and cache separation. Promotion progress: art remains
experimental; no game asset has been admitted by this repair.
