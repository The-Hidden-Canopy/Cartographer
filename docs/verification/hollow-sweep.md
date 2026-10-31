# Hollow sweep authoring

The wires/pipes/cables/hoses backlog now has a hollow geometry primitive in
the existing native sweep authoring path. This is source geometry and derived
preview work; it does not simulate flow, pressure, cable mechanics, or hose
behavior, and does not add fittings, supports, branches, or wall penetrations.

## Source and geometry contract

`SweepSource::inner_profile_scale` defaults to zero, retaining the solid
sweep. A value strictly between zero and one creates an inward-facing inner
shell by scaling the convex profile about its origin. The origin must lie
strictly inside the outer profile. This is a homothetic inset; it does not
promise uniform perpendicular wall thickness for arbitrary polygon profiles.
Circular radial profiles have the corresponding inner/outer radius ratio.

The existing transported frames, centerline identity/revision checks, taper,
and cap policy remain in use. End caps on hollow sources become annular rim
quads joining the two shells, leaving the bore open. Open/start/end/both rim
policies preserve their explicit boundary loops. Both shells and every rim
face count toward the output budget before sampling/generation.

Admission refuses nonfinite or out-of-range inner scales, degenerate inner
profiles, outside/boundary origins, and unresolved wall thickness. Final mesh
validation remains mandatory; generated meshes never replace the source.
Global self-intersection, bend-radius compliance, manufacturing suitability,
and physical behavior are not inferred from topological validity.

Solid sources serialize with exactly the existing version-1 layout and no
additional field. Hollow sources use nested source version 2 with the
mandatory `INNER_SCALE` field. Version-1 decoding remains supported. Version-2
zero/negative or malformed scales and trailing records refuse. This feature
does not migrate or rewrite existing project files; older readers that only
support sweep source version 1 must refuse newly authored hollow records.

The native radial editor loads and preserves the hollow parameter and offers
an explicit inner-scale control. A loaded source must still match its captured
source revision and project generation before update; a changed source needs
an explicit reload. Publication remains the existing human-only
`SetSweepSourceAction`, editor command, project history, and durable journal
path. The preview remains read-only.

## Evidence

The desktop and focused native targets built successfully. The selected CTest
run passed 12/12 (17.75 s); after the last application/UI links completed,
those two suites passed again (2/2, 22.55 s). Kernel coverage checks deterministic IDs/positions,
legacy/source-v2 codec behavior, annular cap topology, exact signed material
volume for a square tube, each rim policy, invalid walls, stale centerlines,
output bounds, and curved/tapered frames.

Executed project and public UI coverage checks journaled source
creation, no-mutation invalid/stale refusal, human admission, pure preview,
undo/redo, and saved-project reopen preserving the inner scale. The current
precision-loss and connected-shell cases passed. Native pointer interaction and rendered hollow previews
are not claimed from the build alone.

## Change ledger

- `libs/geometry/mesh/include/carto/geometry/sweep.hpp` and `src/sweep.cpp`:
  source version 2, wall validation, inner shell, annular rims, and budgets.
- `apps/desktop/main.cpp`: radial hollow controls, loaded-source preservation,
  and revision/project-generation admission for edits.
- `tests/procedural_sweep_tests.cpp`, `tests/sweep_source_project_tests.cpp`,
  `tests/sweep_endpoint_tests.cpp`, and `tests/ui_tests.cpp`: geometry, codec,
  transaction, endpoint digest lineage, and public UI tests. Legacy project
  fixture trimming now targets the outer `END`, preserving nested graph bytes.
- `libs/application/include/carto/application/application.hpp`: a separate
  compile integration fix adds the affected-mesh receipt field already used
  by the in-progress prefab rebase command. No new mesh authority is granted.
- `tests/condition_prefab_placement_tests.cpp`: compile/fixture corrections use
  the whole application snapshot for project fields and select the exact
  authored prefab identity; the rebase receipt must include removed/retained
  mesh IDs. Separate connection lineage coverage is described in
  [connection edge history](connection-edge-history.md).
- Sweep architecture/verification documentation and the ignored task queue:
  capability and remaining gates.

Capability gained: a revision-bound authored sweep can represent an actual
bore and wall rather than a solid rod, while retaining editable source,
deterministic rebuild, and the existing publication boundary.

Invariants preserved: stable local topology IDs, exact source/path revisions,
explicit unsupported diagnostics, bounded work, no source mutation during
preview, human-only authoring, monotonic history, journal admission, and
save/reopen. Org scopes, role tiers, and datetimes are not part of this local
geometry contract; existing application authority checks remain unchanged.

Risks: hollow meshes require approximately twice the side vertices/faces plus
rims. Large-project/frame cost is unmeasured. Sweep source version 2 requires
a reader update, and the appended C++ fields require consumers to rebuild.
Legacy solid bytes remain unchanged. Native stale-draft controls are built
but still need interaction acceptance.

Rollback: selectively revert the hollow source/codec/builder/control hunks and
their new regressions, preserving other shared workspace work. Preserve
backups of projects containing version-2 sweeps before using an older reader;
do not silently drop the inner shell to make them load. The affected-mesh
receipt compile repair is separate from hollow geometry and should be reverted
only together with its prefab callers if desired.

Rejected: filled end disks hiding the bore, two disconnected closed solids,
guessed wall offsets for an outside origin, permissive numeric coercion,
budgeting only one shell, implicit source replacement, physics claims, and
closing the complete conduit/fitting task from one geometry feature.

RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA were not edited. No shared
API or schema with those products was identified; consumer integration and
their suites remain outside this run. No network, deployment, dependency
installation, or remote mutation is needed.

Dataset progress: unchanged. Training/prompt progress: unchanged. Capability
progress: hollow authored geometry and bounded regression coverage. Promotion
progress: local feature evidence only; native/consumer/production gates remain.

The current tree/index and selected-`HEAD` publication audit with ignore-policy
checking returned `PUBLIC_BOUNDARY_PASS`. Scoped `git diff --check` passed.
The full configured suite, native interaction, external product suites, and
broader public refs were not checked in this continuation; they are separate
acceptance scopes. Build warnings remain in concurrent prefab snapshot and
desktop thumbnail/material code; no compile/link errors remain in these targets.

## Persisted sweep owner binding (2026-10-27; code-only)

`SweepSource` nested version 3 can bind its centerline and generated local mesh
to a nonzero scene object ID. The project transaction and full document
validation require that owner to exist. `SweptMeshProduct` carries the owner ID
and the application preview resolves the owner's current world transform,
project revision, and scene revision into its transient result. The desktop
wireframe applies that transform for display and camera framing, and marks an
owner-bound preview stale after any project revision change. Ownerless version-1
solid and version-2 hollow serialization remains unchanged; version 3 carries
an explicit owner block and an `INNER_SCALE` value, including zero for a solid
owned sweep. The outer project schema does not change.

- **Files changed:** `libs/geometry/mesh/include/carto/geometry/sweep.hpp`,
  `libs/geometry/mesh/src/sweep.cpp`, `libs/project/src/project.cpp`,
  `libs/application/src/application.cpp`, `apps/desktop/main.cpp`,
  `tests/sweep_source_project_tests.cpp`, `tests/application_tests.cpp`, this
  ledger, and the ignored local queue.
- **Capability gained:** a persisted sweep records the scene object whose local
  coordinate frame owns its generated geometry; the application preview
  returns the current world transform with project/scene revision provenance.
- **Boundary improved:** a missing owner fails with `not_found` at project
  admission and invalid documents cannot retain a dangling owner reference;
  the desktop hides owner-bound preview geometry if its project revision is
  stale and applies the exact returned owner transform when drawing.
- **Governance preserved:** sweep authoring still publishes through the
  existing project transaction and journal. Geometry building remains pure;
  no snapping, fitting placement, connection mutation, or authority was added.
- **Tests authored:** v3 source round-trip through project save/reopen, build
  product owner retention, missing-owner refusal with transaction rollback, and
  application preview transform/revision binding. **Run:** none, at the user's
  instruction. Desktop rendering and frame behavior have no exercised evidence.
- **Tests not run:** no build, geometry/project/application suite, or downstream
  adapter test or native viewport interaction.
- **Risks:** the owner ID follows the current scene object's transform; the
  source does not bind a transform snapshot or resolved common-frame receipt.
  Endpoint spatial alignment, fittings, supports, branching, and native review
  remain unimplemented. Older readers that only support v1/v2 must refuse an
  owner-bound v3 record. Downstream aggregate initializers require rebuilds.
- **Rollback:** revert the nested v3 owner field/codec, project owner checks,
  product propagation, focused regression source, and this continuation. No
  outer project schema migration or data rewrite is required.
- **Rejected:** treating compatibility tags as spatial alignment, embedding a
  frozen world transform that would stop following its owner, or creating
  fittings from guessed endpoint positions.
- **Cross-product trace:** direct impact is Cartographer geometry and project
  source persistence only. RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA
  have no shared contract changes. Dataset/evaluation state is unchanged;
  capability is code-only; deployment and promotion remain open.
- **Required follow-up:** build and run sweep geometry, project, application,
  endpoint, and native UI suites; then bind resolved endpoint positions/axes to
  the same project/scene snapshot before spatial alignment or fitting work.

## Radial-editor owner retention (2026-10-27; code-only)

The compact radial editor rebuilds an update candidate from its controls. It
now copies the existing owner object only when the exact source identity and a
newer source revision are bound to the loaded editor state. Ownerless sources
remain ownerless; new records created by this editor remain in project
coordinates. The source list and editor identify the current frame so an
owner-local sweep is not mistaken for project-space geometry.

- **Files changed:** `apps/desktop/main.cpp`,
  `apps/desktop/studio_sweep_source.hpp`,
  `tests/desktop_sweep_source_tests.cpp`, `tests/CMakeLists.txt`, this ledger,
  and the ignored local queue.
- **Capability gained:** editing a loaded owner-bound solid or hollow sweep
  retains its source-to-owner frame through the normal journaled update action.
- **Boundary improved:** stale loads, mismatched identities, and non-advancing
  revisions cannot rebind an owner; new editor-created sources do not inherit a
  prior source's owner.
- **Governance preserved:** the helper only copies source metadata. Publishing
  still uses the existing human `SetSweepSourceAction`, project history, and
  journal; preview remains read-only.
- **Tests authored:** exact owner-bound update, ownerless update, and stale or
  mismatched source refusal without changing candidate ownership. **Run:** none,
  per the user's code-only instruction.
- **Tests not run:** CMake configuration, build, desktop interaction, sweep
  project/application suites, and save/reopen acceptance.
- **Risks:** the editor still creates new ownerless sweeps and cannot change an
  existing owner binding. This change does not establish endpoint placement,
  fitting alignment, or native interaction behavior.
- **Direct impact:** Cartographer's radial sweep source update form. **Indirect
  impact:** owner-bound previews keep resolving the saved source's coordinate
  frame after a human edit.
- **Shared contract/API/schema risk:** reads the existing
  `SweepSource.owner_object_id` field and `SetSweepSourceAction`; no public
  service API or serialized schema changes. Existing nested schema v3 readers
  still need to support owner-bound sources.
- **Dataset/evaluation risk:** none; no dataset or evaluation artifact changed.
- **Deployment risk:** desktop compilation and packaged UI behavior are
  unverified.
- **Required follow-up tests:** build and run
  `cartographer_desktop_sweep_source_tests`, then the sweep project/application
  suites; exercise exact owner-bound update, project save/reopen, preview frame,
  and stale-load refusal in the native UI.
- **Rollback:** remove the owner-retention helper and its target, remove the
  exact-update call and frame labels from `apps/desktop/main.cpp`, then remove
  this section. No saved project migration is needed.
- **Rejected:** silently dropping the owner on update, inheriting an owner's
  frame for a different/new source, or adding owner selection without a proven
  local-coordinate authoring workflow.
- **Cross-product trace:** Cartographer only. RegOS, IDA_Training,
  hidden_canopy_bot, and Ask_IDA contracts and datasets are unchanged; dataset
  and training progress are unchanged, capability progress is code-only, and
  promotion progress remains open pending compile, focused execution, and
  native acceptance.

## New sweep owner selection and desktop target wiring (2026-10-27; code-only)

The radial editor now lets a human bind a new sweep source to the single
currently selected scene object or keep it in project coordinates. The editor
states that an owned centerline is interpreted in that object's local frame
and that it does not convert authored coordinates. The owner is rechecked
against the current scene snapshot and project generation when saving, and the
choice resets after a successful save. Updates still preserve only the exact
loaded source owner. Using the active selection avoids walking every scene
object in the UI frame.
The helper header is now listed in the desktop executable sources for
generated-project visibility.

- **Files changed:** `apps/desktop/main.cpp`,
  `apps/desktop/studio_sweep_source.hpp`, `apps/desktop/CMakeLists.txt`,
  `tests/desktop_sweep_source_tests.cpp`, this ledger, and the ignored local
  queue.
- **Capability gained:** new sweep records can use the existing owner-local
  preview frame from the native editor. The desktop target source list now
  includes the already-included `branding.hpp`, `studio_scene_camera.hpp`, and
  `studio_sweep_source.hpp` headers for generated-project visibility.
- **Boundary improved:** owner selection is limited to a current selected
  object and new identities; a project switch invalidates the pending owner
  ID, and source updates cannot silently change frame. The UI calls out that
  coordinates are not transformed.
- **Governance preserved:** mutation remains a human
  `SetSweepSourceAction` through project history/journal; owner selection and
  preview do not add a second mutation route.
- **Tests authored:** current owner assignment, ownerless selection,
  disappeared-owner refusal, project-generation mismatch, attempted rebind
  refusal, and prior exact-update cases. **Run:** none, per the user's code-only
  instruction.
- **Tests not run:** CMake configure/build, focused or project suites, save and
  reopen, and native UI interaction.
- **Risks:** the new choice intentionally interprets the selected centerline
  coordinates as owner-local without converting them; a user who authored them
  in project coordinates must leave the source ownerless. Compilation and
  interactive behavior are unverified.
- **Rollback:** remove the new owner-selection helper and UI state/control,
  remove `branding.hpp`, `studio_scene_camera.hpp`, and
  `studio_sweep_source.hpp` from the desktop source list, and remove the new
  regression cases and this section.
- **Rejected:** silently transforming existing path coordinates, changing the
  owner of an existing source, or claiming native acceptance without execution.
- **Cross-product trace:** direct impact is Cartographer's sweep editor;
  indirect impact is limited to owner-bound previews following the selected
  scene object's transform. RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA
  contracts are untouched. No public API or project schema changed. Dataset,
  training, deployment, and promotion progress are unchanged; capability
  progress is code-only.
- **Required follow-up tests:** compile the desktop target and run
  `cartographer_desktop_sweep_source_tests`, then exercise create/update,
  preview following the current owner transform, stale owner refusal, and
  save/reopen in the native editor.

## Owner-bound endpoint admission (2026-10-27; code-only)

The read-only sweep endpoint binding now rejects a connection port whose scene
object owner differs from a nonzero `SweepSource.owner_object_id`. Ownerless
legacy sources retain the previous compatibility-only behavior. This closes a
record-identity mismatch while leaving frame resolution, spatial alignment,
fitting placement, and graph mutation unimplemented.

- **Files changed:** `libs/condition_authoring/src/sweep_endpoint.cpp`,
  `tests/sweep_endpoint_tests.cpp`, this ledger, the sweep architecture and
  acceptance-matrix rows, and the ignored local queue.
- **Capability gained:** owner-bound sweep endpoint checks can distinguish a
  port authored on the sweep's owning scene object from a foreign object's
  port.
- **Boundary improved:** a valid port ID and compatible tags no longer suffice
  when the source carries a conflicting scene-object owner.
- **Governance preserved:** the check is read-only; no scene/project mutation,
  snapping, fitting, connection edge, or spatial-alignment claim was added.
- **Tests authored:** matching owner acceptance, foreign owner refusal, and a
  compatible pair whose endpoints each match their distinct owners. **Run:**
  none, per the user's instruction.
- **Tests not run:** no build, endpoint suite, application suite, or native
  interaction.
- **Risks:** ownerless legacy sources still have no spatial binding; owner
  equality does not prove that the port frame lies at the curve endpoint.
- **Rollback:** revert the owner comparison and its regression case, then
  restore the preceding endpoint contract text in this ledger and the two
  contract tables.
- **Rejected:** inferring endpoint alignment from owner identity or treating
  compatibility evidence as placement evidence.
- **Cross-product trace:** direct impact is Cartographer's endpoint admission
  contract. RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA have no shared
  contract changes. Dataset/evaluation and deployment are unchanged; promotion
  remains code-only and unverified.
- **Required follow-up:** run the endpoint and connection-port tests, then
  resolve both port frame chains and sweep endpoint positions from the same
  project/scene snapshot before implementing spatial alignment.

## Read-only endpoint spatial review (2026-10-27; code-only)

The compatibility contract proved tag/capability agreement and owner identity,
but it did not measure whether a saved curve endpoint met its port. A new
read-only review samples the exact revision-bound path, resolves the port's
same-owner parent-frame chain, composes that frame with the current scene
transform, and reports endpoint-to-port distance plus tangent alignment to an
explicitly selected port-local outward axis. Its receipt binds the source,
centerline, port, every resolved frame record, project/scene revisions, and the
caller's numeric policy. Distance and axis tolerances come from the caller;
the system does not infer a physical convention.

`ApplicationSession` refuses stale project snapshots and resolves current
project records before producing the review. `UiController` exposes that path,
and the desktop sweep panel provides endpoint side, port ID, local axis, and
tolerance controls with a visible measurements/result summary. The UI labels
distance in project units and states that the review creates no graph edge or
fitting.

- **Files changed:**
  `libs/condition_authoring/include/carto/condition/sweep_endpoint.hpp`,
  `libs/condition_authoring/src/sweep_endpoint.cpp`,
  `libs/application/include/carto/application/application.hpp`,
  `libs/application/src/application.cpp`,
  `libs/ui/include/carto/ui/ui.hpp`, `libs/ui/src/ui.cpp`,
  `apps/desktop/main.cpp`, `tests/sweep_endpoint_tests.cpp`,
  `tests/application_tests.cpp`, this ledger, and the ignored local queue.
- **Capability gained:** a user can inspect a named sweep endpoint's spatial
  relation to its current port and receive deterministic, revision-bound
  evidence before a fitting or attachment workflow exists.
- **Boundary improved:** missing, ambiguous, cyclic, stale, or foreign-owner
  frame bindings refuse; position/axis mismatch returns measurements with
  `aligned == false` instead of moving either record.
- **Governance preserved:** this is read-only. Graph edits still use the
  existing human command and project history; no background mutation, snap,
  fitting creation, attachment state, or physical compliance claim was added.
- **Tests authored:** exact owner-frame fit, parent-frame composition,
  deterministic digest, stale centerline, ambiguous/missing parent, position
  gap, reversed axis, invalid policy, application snapshot binding, and
  no-mutation behavior. **Run:** none, per the user's instruction.
- **Tests not run:** no build, condition/application/UI suite, or native desktop
  interaction. The new regression source and desktop panel are unverified.
- **Risks:** distance units follow authored project coordinates. The policy
  axis meaning and thresholds remain explicit caller choices. Transform
  composition follows Cartographer's existing `Transform` model; shear from
  nested nonuniform scale/rotation is not modeled. A fit review is not a
  mechanical, flow, or manufacturing acceptance receipt.
- **Rollback:** selectively revert the spatial review types/implementation,
  ApplicationSession and UiController forwarding methods, desktop panel state
  and controls, new regression functions, and this ledger section. Existing
  compatibility-only endpoint checks, owner binding, and source serialization
  remain intact.
- **Rejected:** deriving an axis from tag names, guessing a tolerance or unit,
  snapping source/port transforms, generating a fitting from incomplete
  geometry, and treating distance-only evidence as a completed connection.
- **Cross-product trace:** Cartographer only. RegOS, IDA_Training,
  hidden_canopy_bot, and Ask_IDA contracts and datasets are unchanged. Dataset
  and training progress are unchanged; capability is code-only; deployment,
  native acceptance, and promotion remain open.
- **Required follow-up:** build and run condition, application, UI, and desktop
  suites; exercise exact and mismatched endpoints with saved projects; inspect
  the native panel; then separately define and review conduit/fitting geometry
  and any graph-mutation acceptance.
