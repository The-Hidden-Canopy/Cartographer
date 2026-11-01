# Autorig owned-key prerequisites - source handoff

This continuation is on `codex/autorig-durable-acceptance-20261031` atop
`59a9ecd`. The source below is a bounded implementation step, not acceptance
of a complete FK-to-IK switch, native editor workflow, or runtime effect.
The integration owner must record the exact reconciled commit and validation
results separately.

## Two source owners

`carto_workspace` owns a separately serializable `CharacterSourceStore` and
`WorkspaceController` with its own command journal. A character bundle carries
its rig and graph, optional motion document and one temporal channel. The
`ProjectDocument` and `ApplicationSession` own the accepted-autorig record,
unadmitted mode-key draft, application command history and separate journal.
The inspected project, application, editor and desktop sources expose no
shared authority bridge connecting these owners. Matching numeric IDs or
digests does not itself establish that a workspace rig is the accepted project
rig, or make separate journals one atomic decision.

The unadmitted project draft remains a raw proposed curve and caller assertions.
Its source pose digest is asserted and the portable record excludes the
computed match. Different supplied locals can yield a different successful
match for the same record; it does not establish that a human reviewed that
same match. Its checked project and rig envelope is historical insertion-time
integrity evidence, not authentication or current physical state.

## Control frame proposal

The bounded `ik_control_frame` proposal uses an explicit caller-asserted rigid
transform, frame identity, unit identity and authoring digest. It encodes the
graph-evaluation-frame IK target as a control-frame point and the pole as a
control-frame direction, decodes them, and reruns the existing two-bone solve
and position-jump checks. Its narrow supported control contract requires
explicit XYZ-only target and pole controls, no follow/follower connections,
no affected joints and no scale. Unsupported arrangements refuse. The frame,
units and supplied FK locals are not authenticated, and the proposal does not
prove that those locals came from current authored controls. Position checks
do not establish orientation or time-adjacent continuity. No world scene
transform, human admission, physical contact or runtime action follows from
the result.

## Workspace tracks and atomic edits

Workspace store schema v2 adds bounded, sorted, unique raw `(control, axis)`
tracks with exact rational ticks, explicit component role and asserted frame,
source, rig and graph pins, and canonical curve digests. The current track
codec admits **step curves only**; other interpolation is explicitly unsupported
until its locale-stable representation and migration are specified. Existing
temporal channels are retained separately; no channel or layer equivalence is
implied. Roles and frame values are authoring assertions, not validated target
or pole mappings. Mutable `store()` and `find()` remain historical APIs, so
these rows are not an unbypassable authority boundary.

The v2 store reader accepts v1 as absent tracks; v1 and v2 table fences reject
mislabelled representations. Migration changes the whole-store digest. Old
proposal pins must be rebuilt or explicitly revalidated; source inspection did
not establish automatic comparison of a proposal's old source digest before
its commands are applied. A near-64 MiB v1 store may exceed the v2 encoded
limit after migration and is refused rather than silently truncated. This is
not an export or proof that the old project has been migrated on disk.

The bounded multi-track workspace command checks the expected store revision
and digest plus each curve digest, applies sorted unique inserts or exact
erases to a working copy, validates the full candidate and appends one journal
entry before publication. Exact erase compares stored timebase and value and
tangent bits, including signed zero. Its in-session undo retains the inverse
until the forward undo entry commits; a content fingerprint rejects drift and
allows older LIFO undo after later content is restored. The fingerprint and
digests are integrity checks, not author authentication. Public mutable store
access can still bypass the controller; safe owner enforcement remains open.
The replay source test checks restored store content and revision, not
reconstruction of the undo stack after restart.

Existing scalar `animate_set_key` is a separate temporal-channel path: it
converts rational time to floating-point seconds. Its reference refresh does
not update motion-document channel or clip digest references, and the bundle
validates those records separately. Do not treat a changed channel and old
reference as a current reconciled motion document. The legacy channel's
non-step serialization can also depend on ambient locale; no cross-locale
authority is established by these changes. Scalar key overwrite undo and
failed-undo retention receive bounded repairs in this continuation; they do
not make that floating-point path exact-tick authoring.

## Requirement and review boundary

`IK-001` requires typed constraint authoring, ordering and limits; `IK-004`
requires editable match, pole, aim and space controls with a no-pose-jump
native switch. `AUT-004` requires a reviewed native candidate to remain
non-mutating until acceptance. `ANI-002` requires human key, tangent and
retime authoring with undo and visual proof. The character production
docuseries chapter 06 requires typed many-to-many control/space relationships;
chapter 10 requires staged multi-contact solving, residuals and exact-time
switch continuity; chapter 13 requires native timeline and layer editing.
The source components here do not close those gates.

Independent reviewers `/root/review_previous` and
`/root/control_trace/mode_api_trace` examined the workspace track and batch
paths; `/root/claims_trace/field_trace` examined the control-frame mapping.
Those reviews identified the finite-component pole-norm overflow,
locale-dependent non-step curve portability, whole-store size bound, undo and
codec fence issues; the bounded changes incorporate the corresponding source
repairs or explicit refusals. Review is not execution evidence.

The source tests `ik_control_frame_tests.cpp`,
`workspace_control_axis_track_tests.cpp`,
`workspace_control_track_batch_tests.cpp`, and the focused workspace undo
regressions are authored but **not compiled or run** for this continuation.
The prior global build-coordinator lease remains recorded active; this lane
has no compile/build grant. Integration must run
the focused suites, v1/v2 migration and journal replay regressions on the exact
candidate and inspect any remaining locale and source-reference cases.

## Remaining admission path

A real owned FK-to-IK switch needs a human-reviewed, current source-pose and
proposal token bound to the exact workspace/project identities, rig/rest/limit
and frame inputs, exact tick and full candidate result. It needs a deliberate
cross-owner bridge with one atomic journal/undo decision, or an explicitly
chosen single source owner and migration; the present independent journals
cannot promise this. Typed control-space decoding must be integrated with the
actual evaluated control graph, including follow spaces, limits and affected
joint behavior, then re-evaluated against authored target/pole/mode tracks and
measured on both sides of the switch. Multi-chain arbitration, hard-contact and
limit reconciliation, native timeline controls, independent asset evidence,
VANTA owner admission and runtime parity remain separate gates. Authoring undo
is not physical time reversal, and source intent is not observed or executed
state.

## Source change counts

Counts against `59a9ecd` are added/deleted lines. New files were untracked before staging.

| Path | Added | Deleted | Before staging |
| --- | ---: | ---: | --- |
| `docs/verification/autorig-owned-key-prerequisites-20261101.md` | 152 | 0 | new |
| `libs/pose/CMakeLists.txt` | 1 | 0 | tracked |
| `libs/pose/include/carto/pose/ik_control_frame.hpp` | 43 | 0 | new |
| `libs/pose/src/ik_control_frame.cpp` | 158 | 0 | new |
| `libs/workspace/CMakeLists.txt` | 1 | 0 | tracked |
| `libs/workspace/include/carto/workspace/commands.hpp` | 27 | 1 | tracked |
| `libs/workspace/include/carto/workspace/controller.hpp` | 8 | 1 | tracked |
| `libs/workspace/include/carto/workspace/store.hpp` | 30 | 1 | tracked |
| `libs/workspace/src/commands.cpp` | 164 | 3 | tracked |
| `libs/workspace/src/controller.cpp` | 67 | 9 | tracked |
| `libs/workspace/src/proposals.cpp` | 3 | 2 | tracked |
| `libs/workspace/src/store.cpp` | 138 | 1 | tracked |
| `tests/CMakeLists.txt` | 15 | 0 | tracked |
| `tests/ik_control_frame_tests.cpp` | 124 | 0 | new |
| `tests/workspace_control_axis_track_tests.cpp` | 164 | 0 | new |
| `tests/workspace_control_track_batch_tests.cpp` | 437 | 0 | new |
| `tests/workspace_tests.cpp` | 162 | 0 | tracked |
