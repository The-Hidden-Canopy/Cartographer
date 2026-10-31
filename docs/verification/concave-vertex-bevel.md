# Simple concave vertex-bevel caps

## Capability change

The bounded closed triangular-fan vertex bevel now accepts a generated cap
that is simple and planar even when its boundary is concave. Single-segment
caps remain one polygon face; the existing mesh compiler triangulates that
face. Multi-segment concave caps are deterministically ear-clipped after the
existing bounded boundary subdivision. Convex multi-segment caps retain their
previous center-fan topology so existing schema-v1 bevel patches keep the same
result geometry and replay digest.

The operation still requires a closed internal triangular fan with 3..64
incident faces, a finite positive width below half the shortest incident edge,
and 1..64 segments. Non-simple and non-planar caps, boundary vertices,
non-triangular fans, non-manifold topology, and collapsing widths continue to
refuse atomically. This does not add arbitrary fan topology or adjustable
profiles.

Concave caps are derived from simple planar concave source boundaries. Equal
world-distance cuts would warp those caps because they lie on a sphere, so the
concave path applies one shared edge fraction; `width` remains the cut distance
on the shortest incident edge. The legacy per-edge distance rule remains for
convex boundaries, preserving existing schema-v1 replay geometry.

## Repair ledger

- **Files changed:** `libs/geometry/mesh/include/carto/geometry/mesh.hpp`,
  `libs/geometry/mesh/src/mesh.cpp`, `tests/topology_edit_tests.cpp`,
  `tests/application_tests.cpp`,
  `docs/roadmap.md`, `docs/verification/acceptance-matrix.md`, this ledger, and
  the ignored local work queue.
- **Capability gained:** human topology authoring can bevel a closed triangular
  fan whose generated planar cap is simple and concave, including bounded
  multi-segment caps.
- **Boundary improved:** deterministic simple-polygon triangulation replaces a
  blanket convexity refusal; non-simple and non-planar geometry remains
  fail-closed. Regression source covers single- and three-segment concave caps,
  a self-crossing fan refusal with no mutation, valid topology/receipt lineage,
  and successful mesh compilation. Application source covers the human tool
  action, patch replay, undo/redo, save/load, and reopened viewport geometry.
- **Geometry correction:** source review showed equal-distance cuts cannot
  produce the documented planar concave cap from the authored planar fan
  fixture. The new-input path now uses a shared edge fraction only for a simple
  planar concave source boundary. Convex inputs preserve their prior cut
  positions and replay digest.
- **Governance preserved:** the edit remains a bounded deterministic geometry
  operation. Projected writes continue through existing revision-bound patches
  and human commands. No schema or patch format changed. The AI capability's
  existing convexity validator and preview gate were deliberately retained;
  kernel support does not expand AI mutation authority.
- **Tests authored:** topology source now accepts and compiles simple concave
  caps at one and three segments, refuses a self-crossing fan atomically, and
  checks the concave cap plane and shortest-edge cut distance while preserving
  existing convex segmented receipt counts. Application source also exercises
  the concave path through persisted history and reopen.
  **Run:** none, per the user's no-test instruction.
- **Tests not run:** builds, topology/application/AI suites, the authored
  save/reopen and undo/redo regression, native viewport input, and rendered
  review.
- **Risks:** the changed C++ source is uncompiled. Proportional cuts may produce
  visibly wider cuts on longer incident edges for concave boundaries; this new
  behavior is documented and remains unreviewed. Ear clipping scales
  quadratically with cap boundary length, bounded here by 4,096 samples; worst-
  case edit cost and generated topology remain unmeasured. Concave patch replay,
  save/reopen, and receipt digests are not verified.
- **Rollback:** restore the convexity refusal and center-fan-only path in
  `mesh.cpp`, remove the concave-only proportional-cut branch, restore the
  matching API/acceptance descriptions and source regression expectations,
  remove the concave application fixture and route from
  `tests/application_tests.cpp`, and remove this ledger entry. Existing
  workspace changes outside these hunks remain untouched.
- **Rejected:** broadening AI admission, changing persistent patch schema,
  changing convex bevel output, retaining equal-distance cuts for a planar
  concave cap, triangulating non-simple caps, or claiming replay/native
  acceptance from source inspection.

## Cross-product and promotion trace

Direct impact is Cartographer geometry; indirect impact is the existing human
application command and its patch/history persistence path. The shared
`BevelVertexPatch` schema and API remain unchanged. RegOS,
IDA_Training, hidden_canopy_bot, and Ask_IDA have no code, API, schema, data,
evaluation, or deployment changes. Dataset and training/prompt progress are
unchanged. Deployment risk is limited to compiling and integrating the changed
Cartographer C++ sources. Capability progress is source-only. Promotion
remains open pending compilation, topology and application execution, patch
replay/save/reopen evidence, native viewport observation, and visual review.

## Code-only desktop input guidance (2026-10-26)

The vertex-bevel control now explains the shape-dependent width semantics:
concave planar caps use one shared fraction, so the entered width is the cut
distance on the shortest incident edge; convex caps retain per-edge distance.
This surfaces the kernel's actual behavior beside the human-authored input.
No build or test was run. Native viewport review remains open.
