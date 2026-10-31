# Sculpt and garment authoring verification

`cartographer_production_authoring_tests` covers the public production layer:

- SculptStack serialization and canonical digest round-trip;
- bounded surface samples, mask/settings/displacement references, symmetry,
  parent-cycle rejection, hostile trailing/future input, and stale publication;
- fabric intent, panel outline/rest-shape identity, seam edge bounds, body-bound
  pins, constraint validation, and representation-tier uniqueness;
- GarmentAsset serialization and canonical digest round-trip;
- DrapeRequest and DrapeReceipt source/garment/request/iteration binding;
- rejection of a receipt produced for a changed garment;
- a negative-space check that public garment records do not carry runtime pose,
  handles, or telemetry.

These tests cover public authoring records and opaque prepared-product receipts.
They do not exercise private evaluators, a downstream runtime, GPU execution, or
a presented cloth frame.

## Sculpt layer controls code continuation (2026-10-25; unverified)

The Sculpt workspace now exposes weight entry and adjacent layer reordering in
addition to visibility. `ApplicationSession` applies all three through the
existing `SetProjectSculptStackCommand`, after checking the selected unlocked
mesh, project revision and generation, layer identity, human operation source,
weight range, and final destination index. Stack serialization already carries
weight and vector order, so this adds no project schema or patch-format change.
UI operation tracking includes weight and order edits.

An authored UI regression covers a zero-weight layer, creation of a second
layer, reordering, invalid weight and destination refusal, stale project
generation/revision, raw-AI refusal, undo/redo, and save/reopen. No builds or
tests were run in this code-only sprint. Native Sculpt click-through, brush
evaluation, GPU/CPU parity, and the remaining production acceptance gates stay
open. Rollback is limited to the three layer actions/helper, Sculpt panel
controls and operation accounting, the authored test, and this evidence update;
the serialized SculptStack format is unchanged.

## Sculpt stroke snapshot fencing continuation (2026-10-25; unverified)

Point and viewport-surface SculptStack stroke actions now carry both the
project generation and revision. The application rejects either mismatch
before resolving selection, mesh, or surface anchors. Native surface-stroke
capture records the generation at pointer-down and cancels if it changes while
the gesture is in progress; the commit repeats the same fence. This closes a
same-revision project-replacement gap without changing stack or project
serialization.

Regression coverage was updated for exact-generation happy paths and added
same-revision wrong-generation refusal for both point and surface strokes. The
tests are authored but were not run, and no build was run. Native capture,
GPU/CPU parity, and broader production acceptance remain unverified. Rollback
is limited to the two action fields/checks, native capture generation field and
guards, call-site updates, and this evidence note.

## SculptStack public/private boundary correction (2026-10-25; unverified)

The public application now keeps SculptStack as revision-bound authored source.
Point strokes and surface anchors resolve against the current source mesh;
layer visibility, weight, and order remain human-only journaled edits. The
public viewport compiles the source mesh and does not execute SculptStack brush
kernels. Decal source admission therefore sees the same source mesh after a
stack edit and can still reject an explicitly supplied derived mesh.

The public production target and application no longer reference the local CPU
evaluator, and its test target was removed. The D3D12 test runner's unbacked
SculptStack “PASS” message was removed because no corresponding test function
was invoked. A UI regression now asserts that authored stack edits preserve the
source-only viewport and that a separately supplied deformed display is refused
by source-bound decal admission. These edits were not built or tested per the
active no-test instruction.

The evaluator header/source and its dedicated test source remain as pre-existing
untracked local workspace files; they are not linked by the public build, but
they still require transfer to the verified private intake destination or
owner-directed removal before publication. No public-boundary audit, build,
test, GPU parity, or native capture is claimed for this correction. Historical
exposure and the all-ref publication gate remain open. Project and SculptStack
formats are unchanged.
