# Viewshed observer-source wiring

Code-only continuation, 2026-10-25. The CONDITION Viewshed panel can use either
explicit world coordinates or the current world origin of the single selected
scene object. Object binding stores the project generation and observer ID;
the query also carries that ID so sightline evaluation excludes the observer's
own geometry. The world transform is resolved from the current application
snapshot, including parent transforms.

An object binding from a replaced project generation or an object that no
longer exists refuses evaluation. Invalid world transforms refuse evaluation.
The explicit-coordinate route remains available and rejects non-finite values.
Changing the observer source clears the stored preview. Viewport overlay
currentness also checks the exact captured observer ID and position against the
current source, in addition to terrain digest/LOD, project revision/generation,
and result shape.

## Capability and governance

- Capability gained: operators can anchor a read-only visibility query to a
  selected scene object, and stale local observer inputs cannot leave a prior
  result displayed as current.
- State/audit path: evaluation only; no project state, journal, history, or
  mutation authority is involved.
- Human boundary: selecting or changing the observer is an explicit UI action.
  No automatic camera, terrain, or fallback observer is chosen.
- Shared contract: desktop-only observer/footprint helpers over
  `ApplicationSnapshot` and the existing `LandmarkViewshedQuery`, plus an
  additive read-only projected-surface depth-index API in `carto::ui`. No
  serialized project or service API changes.

## Regression coverage and status

`tests/desktop_viewshed_tests.cpp` covers finite explicit coordinates,
generation-bound object resolution through world transforms, missing/replaced
sources, invalid transforms, and exact observer ID/position mismatch. The test
target is registered in CMake. No test, build, or native interaction was run
under the active code-only sprint instruction.

The desktop overlay also now derives bounded four-corner world-space footprints
from the terrain grid and draws projected quadrilaterals. Unrun helper
regressions cover nonuniform grid spacing, malformed sample counts, degenerate
spacing, out-of-range access, and the sample lattice. Perspective cells are
staged until the current frame's clipped scene triangles are captured. A
64-pixel screen-space index bounds the broad phase to 16,384 buckets and
4,194,304 references; at most 2,000,000 candidate checks and 250,000 sample
points are allowed across the perspective overlay. Each projected terrain cell
uses a bounded 5-to-17 point lattice per axis, increasing density toward an
8-pixel projected edge spacing target. If the project/revision/viewport is
stale, triangles are truncated or malformed, or any work bound is exceeded,
the perspective overlay is hidden. Cells are shown only when every point in
the lattice is uncovered or in front of the nearest captured surface at that
point. This is sampled occlusion and can still miss an occluder narrower than
the sample spacing; it is not a per-pixel depth buffer or a physical visibility
claim. In plan view, the existing geometry-only drawing path is preserved. The
adaptive lattice and sample budget are unbuilt and unrun.
`tests/desktop_viewshed_tests.cpp` covers the bounded lattice endpoints,
center, interpolation, and malformed corners. `tests/ui_tests.cpp` covers
overlapping/missing surfaces, stale triangle spans, invalid viewports, and
index/query budgets; `tests/editor_camera_tests.cpp` covers visible, occluded,
and invalid perspective-depth comparisons.

Risks: compile/runtime compatibility and native control behavior are
unverified. The depth index duplicates screen-space surface lookup state for a
single frame and must be compiled and profiled in dense scenes. Rollback is
limited to the helper, panel state/controls, overlay currentness and depth
checks, regressions, and this evidence note; no project schema or saved
project data changes. The public C++ UI header gains additive index/query
types and functions, so downstream binary consumers must rebuild. Field-of-view
rendering remains terrain-grid geometry-only evidence and does not claim a
rendered or real-world visibility product.

Rejected: an uncapped full-triangle scan per cell and a claimed pixel-accurate
depth buffer. The former has unbounded frame cost; the current viewport API
provides projected triangles but no renderer depth target.

Cross-product trace: direct impact is Cartographer only. RegOS permission,
transition, audit and organization contracts; IDA_Training data/evaluation;
hidden_canopy_bot API; and Ask_IDA query contracts are untouched. Dataset,
training, deployment, and promotion progress are unchanged. Native Viewshed
click-through remains an external acceptance gate.
