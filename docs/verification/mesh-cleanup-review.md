# Bounded mesh cleanup review

The Modify instrument can propose a merge-by-distance cleanup for an explicit
vertex selection on one mesh object. The proposal shows the exact source,
merge tolerance in mesh-local units, removed-to-surviving vertex IDs, modified
and removed face IDs, edge/corner identity changes, and source/result mesh-state
digests, including retained attributes. An operator supplies a reason and
applies or discards the reviewed proposal.

The same review can start from a read-only mesh scan. It reports every vertex
that participates in at least one pair within the selected tolerance, then
builds the same exact merge proposal for human review. Discovery is deterministic
by stable vertex ID and uses the merge kernel's spatial cells and distance test.
It refuses meshes above 100,000 inspected vertices, scans above the configured
pair-check budget, and results above the candidate budget instead of truncating
the findings. A scan never changes the project or selects a repair for approval.

## Source and work bounds

- A proposal requires one visible, unlocked object with a uniquely owned mesh.
- Candidate IDs must be distinct and present. The default and hard maximum is
  4,096 candidates and 4,096 merged vertices; at most 64 removed faces may be
  approved. Callers may lower these budgets.
- Inspection bounds the total source vertices, edges, faces, and corners to
  100,000 before copying or hashing the mesh. Owner inspection also refuses a
  scene above 100,000 objects. Exceeding a bound refuses the entire proposal.
- Supported continuous vertex attributes are equal-weight interpolated across
  each merge cluster. Discrete vertex values are preserved when every source
  value in every merged cluster is exactly equal; conflicting values refuse the
  proposal. Vertex references are checked against the source mesh and remapped
  through the merge receipt; stale references refuse. Edge domains include every
  source edge mapped to a destination edge and transfer only when all values
  agree exactly. Face and corner layers require exact one-source lineage;
  unsupported non-empty domains fail closed. UV seams are not averaged or
  guessed.
- Candidate discovery defaults to at most 1,000,000 pair checks and 4,096
  candidates. A dense cluster that exceeds either bound fails closed.
- Distance comparisons use `std::hypot` so finite extreme coordinates and
  tolerances cannot turn overflow or underflow into an out-of-tolerance weld.
- Corner provenance indexes old face/vertex identities once, avoiding a full
  old-corner scan for each replacement while preserving ambiguous origins.

The existing kernel chooses deterministic survivor identities and validates the
complete result. A failed or empty merge produces a diagnostic and no proposal.
Preview does not modify project data, history, selection, or the journal.

## Human acceptance and lineage

`MeshCleanupReview` is immutable and move-only. Its lifetime token is tied to
the originating application session. It binds project generation/revision,
object and mesh identity, mesh revision, candidate IDs, options, source/result
digests, and the complete topology receipt. Moving, cancelling, or accepting a
token makes the consumed token unusable.

The human adapter requires a nonblank reason of at most 512 bytes without
control characters. Acceptance rechecks current ownership and source state,
recomputes the candidate, and compares both its full mesh-state digest and complete
serialized topology receipt. The reviewed request is applied to a candidate
project using `MergeByDistancePatch`, then admitted through the existing project
command and journal path. The serialized patch retains the exact candidates and
tolerance; the journal retains the operator reason and source/result digests.
This avoids treating a caller-authored digest or patch as mutation authority.

Journal failure rolls back project bytes and command history and leaves the
review available for retry, including an existing redo branch. Durable acceptance
discards the superseded redo branch only after the journal append succeeds.
Successful acceptance clears consumed component
selection and unsupported recent demonstration traces. Undo/redo and project
save/reopen use the existing persistent patch and topology lineage. No project
or journal schema changes are required.

## Acceptance coverage

- `cartographer_topology_edit_tests`: huge and tiny finite inside, exact, and
  outside tolerance cases; deterministic survivors; validated receipts; atomic
  refusal; a 512-triangle duplicate-apex fixture verifies exact indexed corner
  origins and byte-identical receipts under reversed candidate ordering, while
  an ambiguous-origin fixture ensures provenance is not guessed. The merge
  attribute-transfer case covers continuous interpolation, exact preservation of
  matching boolean, integer, string-ID, and object-ID values, edge quotient
  preservation and conflict refusal, exact face/corner lineage, and vertex
  reference remapping with stale-reference refusal. It also covers atomic
  refusal of face insertion and vertex-position patches when derived triangle
  area arithmetic is non-finite.
- `cartographer_application_tests`: read-only review, stable IDs, justification,
  human source, journal rollback/retry with an existing redo branch, cancellation, moved/repeated tokens,
  cross-session and expired-session rejection, stale project revision/generation,
  absent session identity, same-revision geometry drift, shared/locked/hidden
  sources, candidate/removal/work budgets including corner-only overflow,
  persistence and undo/redo.
- `cartographer_tests`: command rollback restores an executable redo branch;
  final acceptance, undo/redo, and history clearing discard rollback backups.
- `cartographer_ui_tests`: explicit controller review followed by human commit,
  operation lineage, refusal without a reason, and consumed-token rejection.
- The Vulkan desktop target compiles the review, reason, apply, and discard
  controls. Native click-through and screenshot acceptance remain to be recorded.

The broader cleanup backlog still includes additional repair types, broader
issue classes beyond nearby vertex pairs and unreferenced vertices, and native
interaction acceptance. Supported discrete and non-vertex transfer now has
explicit equality/lineage rules; unsupported or ambiguous domains still refuse.
The 2026-10-25 code continuations have authored tests but remain unverified
until those tests and native acceptance are run.

### Recorded execution (2026-10-23)

- Built `cartographer_application_tests`, `cartographer_ui_tests`,
  `cartographer_tests`, `cartographer_topology_edit_tests`, and
  `cartographer_desktop` in `build/desktop-vulkan-release`.
- Focused CTest passed all four suites above. The extreme finite-distance
  regression failed against the previous squared-distance comparison before
  the kernel fix and passed after it.
- The public boundary audit passed for the worktree and selected `HEAD` and
  `origin/main` histories, with ignore-policy checks and review failures enabled.
  This does not establish that other public refs or forks are clear.
- Native click-through/screenshots, deployment, a full CTest run, and a native
  responsiveness benchmark were not performed. Compilation and controller
  tests are not substitutes for native acceptance.

## Repair ledger

- Files: geometry discovery in `mesh.hpp`/`mesh.cpp`; provenance transfer in
  `mesh_attributes`; project patch application/replay; application review and
  CMake dependency wiring; UI controller and desktop Modify instrument; the
  topology, application, and UI tests; this note and the local work queue.
- Capability: find bounded nearby-vertex candidates or review an explicit
  selection; transfer supported vertex, edge, face, corner, and single-object
  attributes with exact provenance; accept the reviewed topology through
  durable, reversible authoring history.
- Invariants: read-only proposals; human acceptance; session, source and
  revision binding with full mesh-state digests; complete stable-ID receipts;
  fail-closed ownership and unsupported/ambiguous attribute handling; durable
  append before history finalization.
- Risks: synchronous proposals can pause the UI; the work caps can refuse
  otherwise valid large meshes. Conflicting vertex/edge values, stale vertex
  references, and ambiguous face/corner lineage refuse. Native
  responsiveness and interaction are not yet accepted; the 2026-10-25 changes,
  including the discrete-value continuation, have not been built or tested.
- Rollback: remove the 2026-10-25 discovery and transfer hunks from the geometry,
  mesh-attributes, project, application, UI, test, and documentation files,
  preserving earlier work in those files. There are no migrations or project
  schema changes. Merge patches retain their existing public replay format.
- Rejected: automatic repair, AI approval, silent attribute loss, modifying a
  shared mesh through one owner, guessed corner provenance, and readiness
  claims based only on compilation or unit tests.

### Code continuation (2026-10-25; unverified)

Two-vertex weld and merge-by-distance now preserve discrete vertex values only
after checking all source values for every output vertex. The generic transfer
path then copies the exact equal value; it does not select among disagreements.
Conflicts, missing provenance, and vertex-reference layers without explicit
remapping refused before the candidate mesh was published at this stage. The
topology test now covers equal-value success for both merge routes and the
atomic refusal cases. At that stage ambiguous non-vertex provenance remained
refused. No build or test was run for this continuation at the user's request.

### Code continuation (2026-10-25; unverified)

Merge receipts now derive every source edge that maps to each destination edge
after the vertex quotient. Edge-layer values transfer only when all mapped
sources agree exactly; conflicting or unmapped edges refuse before publication.
Face and corner layers require one exact source, and single-object layers retain
their identity. Vertex-reference values are checked against the source mesh and
remapped through the receipt; zero remains the null reference and stale IDs
refuse. Non-empty unsupported attribute domains remain fail-closed.

Authored topology coverage now includes equal and conflicting edge contributors,
atomic refusal, face/corner lineage, vertex-reference remapping, and stale
reference refusal. Builds and tests were not run per the sprint instruction, so
this is code progress only and does not advance acceptance or promotion. No
project schema or merge-patch format changed. Rollback is limited to the latest
edge-lineage, reference-remapping, and corresponding test/doc hunks in
`mesh_attributes` and `topology_edit_tests.cpp`. No generic averaging or
majority-vote policy was added for contested non-vertex values.

### Read-only issue discovery continuation (2026-10-25; unverified)

The application and UI now expose a separate, read-only inspection report for
unreferenced vertex IDs. The report is bounded by the existing mesh/scene
inspection limits and binds project generation/revision, mesh revision, asset
identity, and the canonical source mesh digest. The Modify panel shows the
stable IDs and clears the report when the project changes. Findings do not
authorize or perform deletion; any future repair must use its own reviewed
topology edit and attribute-transfer path.

Application and UI regression coverage was authored for stable findings,
source binding, no project/history mutation, and refusal on shared, hidden, or
locked sources. No tests, builds, or native interaction were run per the active
code-only instruction. This adds one diagnostic class and does not close the
broader issue-discovery or repair backlog. The report may include vertices that
are intentional standalone data, so its wording is descriptive and it never
implies deletion is required. The fixed 100,000-element inspection cap bounds
the work; larger inputs fail closed. Rollback is limited to the new diagnostic
API, Modify-panel rendering, their regression cases, and the corresponding
documentation hunks; no project schema or patch format changed.

### Duplicate face-cycle diagnostic continuation (2026-10-25; unverified)

The read-only report now groups exact duplicate face cycles by stable face IDs.
Cycle canonicalization is independent of the first corner and winding, so
rotated and reversed copies appear in the same group. The Modify panel renders
at most 32 groups and eight face IDs per group. Discovery remains source-bound
and read-only; it does not remove faces or grant mutation authority.

An application regression was authored for the rotated/reversed case,
deterministic repeated results, and unchanged serialized project bytes and
revision. Tests, builds, and native interaction were not run per the active
code-only instruction. Repair behavior, other topology issue classes, and
native acceptance remain open. No schema or patch format changed. Rollback is
limited to the duplicate-cycle report field and grouping, Modify-panel display,
its regression, and these documentation additions.

### Coincident-position and disconnected-fan diagnostics (2026-10-25; unverified)

The same source-bound report groups distinct vertex IDs at exactly coincident
positions, treating signed zero consistently. The Modify panel displays
bounded stable IDs and labels each finding as read-only. Coincidence does not
authorize a weld. The report also groups incident faces into edge-connected
components at each vertex; more than one component indicates a point contact,
which can be intentional and does not authorize a vertex split.

Application regressions were authored for signed-zero grouping, stable
repeated findings, disconnected components from two triangles that meet only
at one vertex, and unchanged serialized project bytes/revision. Tests, builds,
and native interaction were not run under the
active code-only sprint instruction. The additive application report fields
change source-level consumers only; no project or patch schema changed.
Rollback is limited to the report fields and inspection logic, Modify-panel
rendering, corresponding regression cases, and these documentation hunks.

The mesh validator rejects zero-area triangles before issue inspection. The
candidate point-collapsed-face diagnostic was therefore removed: it could not
be observed through a valid mesh source without weakening that invariant.

### Boundary-edge diagnostic continuation (2026-10-25; unverified)

The source-bound read-only report now lists stable IDs for edges with only one
incident face. The Modify panel shows at most 64 IDs and labels open boundaries
as potentially intentional. The finding does not imply a defect or authorize a
topology change.

An application regression was authored for exact edge identities, repeated
determinism, and preservation of project bytes/revision in the existing mixed
closed/open fixture. Tests, builds, and native interaction were not run at the
user's request. This adds an in-process report field only; no project, journal,
or patch schema changed. Rollback is limited to the report field and scan,
Modify-panel rendering, its regression assertion, and this documentation.

## Cross-product trace and capability delta

- **Direct impact:** Cartographer's read-only application report and native
  Modify panel now expose exact duplicate face-cycle groups, coincident vertex
  groups, disconnected vertex fans, and one-face boundary edges alongside
  unreferenced vertices. A finding does not trigger a repair.
- **Indirect impact:** callers of `MeshCleanupIssueReport` must rebuild against
  the added C++ field. No project, journal, patch, or network schema changed.
- **Shared contract touched:** Cartographer's in-process application report
  only. No RegOS transition/permission/audit contract, IDA_Training dataset or
  evaluation contract, hidden_canopy_bot API, or Ask_IDA query contract changed.
- **API/schema risk:** the report struct has a source-level additive change;
  serialized project compatibility is unaffected.
- **Dataset/eval risk:** no dataset, training, or evaluation material was
  changed. The new regression is engineering coverage and is not promotion
  evidence until run.
- **Deployment risk:** none was deployed. The native target remains unbuilt in
  this code-only continuation, so compile/runtime compatibility is unverified.
- **Required follow-up:** run the focused application and UI suites, build the
  desktop target, then exercise a duplicate-cycle fixture in the native Modify
  panel, including an open-surface fixture. Broader repair types and remaining
  diagnostic classes still need their own review and tests.

Capability gained: operators can see stable face IDs for exact duplicate
cycles whose loop starts or winding differ, groups of coincident vertex IDs,
multiple edge-connected face components at one vertex, and stable edge IDs for
open boundaries. The improved boundary is deterministic, bounded, read-only
source inspection. Governance is preserved because discovery remains tied to
current source identity and cannot mutate the project or bypass human review.
No dataset, training, capability-promotion, or deployment status is advanced.

### Nonplanar n-gon discovery continuation (2026-10-26; code-only)

The source-bound issue report now identifies n-gons whose vertex positions
deviate from the plane through their first three vertices. The relative
deviation is normalized to the lengths of the plane normal and point offset,
and values at or below 64 double-precision epsilons are treated as arithmetic
noise. The Modify panel lists stable face IDs and the maximum measured
deviation. Faces whose finite coordinate differences or reference plane cannot
be represented safely are listed separately as unmeasurable. This is a review
finding, not a decision to triangulate; intentional warped n-gons remain
possible.

- **Files changed:** application report/API and inspection implementation,
  desktop Modify projection, mesh geometry validation and topology regression,
  application regression source, this ledger, and the ignored local work queue.
- **Capability gained:** operators can locate warped polygon faces and compare
  their normalized maximum deviation alongside existing topology findings.
  Mesh admission also rejects triangles whose derived area squared is non-finite.
- **Boundary improved:** planar controls stay unreported; numerically
  unmeasurable reference planes remain explicitly unclassified. Overflowed
  triangle-area values fail atomically in both face insertion and vertex
  position patches. Inspection remains read-only and source-bound.
- **Governance preserved:** no mutation, journal event, project/patch schema,
  permission, or human-review boundary changed. No AI mutation path was added.
- **Tests:** authored warped/planar and extreme-scale-coordinate contrasts,
  deterministic repeated report, and unchanged document/revision assertions;
  topology cases cover atomic rejection of non-finite face area on insertion and
  vertex-position patch. No build or tests were run at the user's request.
- **Risks:** the source changes are uncompiled. The plane is derived from the
  first three ordered vertices and residual arithmetic is approximate within
  the stated epsilon bound; this does not detect self-intersection or prove
  authoring intent. The unmeasurable contrast uses finite, highly disparate
  coordinate scales: raw area remains above the mesh threshold while the
  normalized reference normal underflows. Triangle insertion and position-patch
  admission now refuse non-finite derived area values atomically. This also
  refuses mathematically nonzero triangles whose squared area exceeds the
  representable double range; robust rescaling is not part of this repair.
- **Rollback:** remove `MeshCleanupNonPlanarFace`, its report fields and scan,
  the Modify rows, the geometry-validation guard, the topology/application
  regression cases, and this continuation.
- **Rejected:** treating warped faces as automatically defective, silently
  flattening/triangulating them, claiming complete self-intersection detection,
  or admitting a face whose area cannot be represented safely.
- **Cross-product trace:** Cartographer application/UI only. RegOS,
  IDA_Training, hidden_canopy_bot, and Ask_IDA contracts/data are unchanged;
  dataset, training, deployment, and promotion progress remain unchanged.

The broader mesh-cleanup task remains started; this code-only diagnostic does
not advance native acceptance or promotion.

### Whole-mesh face-component diagnostic continuation (2026-10-26; code-only)

The read-only report now groups all mesh faces into edge-connected components
and exposes the list only when a mesh has more than one component. Stable face
IDs order both the components and their members. The Modify panel shows the
component count and the first face ID and size for a bounded sample; separate
shells are explicitly described as potentially intentional.

An application regression covers an edge-connected duplicate-face pair plus
an isolated triangle, a point-contact pair, deterministic repeated results,
and unchanged project bytes/revision. The assertion was authored but not run.
Builds, tests, and native interaction remain unverified. This finding does not
authorize combining or deleting components and does not change project or
patch serialization.

- Files changed: `libs/application/include/carto/application/application.hpp`,
  `libs/application/src/application.cpp`, `apps/desktop/main.cpp`,
  `tests/application_tests.cpp`, this file, and the local queue.
- Capability gained: operators can inspect disconnected edge-connected face
  islands through the existing source-bound Modify diagnostic.
- API/schema risk: `MeshCleanupIssueReport` gains a trailing source-level field
  and downstream C++ consumers may need rebuilding; project and patch formats
  are unchanged.
- Invariants preserved: inspection remains read-only, deterministic, within
  the existing mesh element budget, and bound to the exact source digest.
- Rollback: remove the report field and traversal, Modify display, regression
  assertions, and this continuation.
- Rejected: treating separate components as defects or automatically merging
  them.
- Cross-product trace: Cartographer only; no other product API, schema,
  dataset, or evaluation contract changed. Capability progress is code-only;
  promotion remains open.

### Boundary component summary continuation (2026-10-26; code-only)

The read-only issue report groups boundary edges that meet at stable vertex IDs.
Each component retains its sorted edge IDs and identifies degree-one endpoint
vertices, degree-greater-than-two junction vertices, and whether the component
forms a closed loop. It also retains the sorted unique incident face IDs, so
the review identifies the adjacent source surface. This makes open chains and
point-joined or branched boundaries reviewable without treating intentional
openings as defects. Existing individual boundary-edge IDs remain available.

The Modify panel shows at most 32 components, eight edge IDs and eight vertex
IDs per row, and eight incident face IDs per component. It explicitly notes
that point-touching boundaries can share a component. Application regression
source covers exact incident faces for a closed triangular boundary and a
point-touching pair with a junction, repeatability, and no project mutation.
No test, build, or native interaction was run under the active code-only sprint
instruction.

- Files changed: the application report/header, its inspection logic, the
  desktop Modify projection, application regression source, this file, and the
  local queue.
- Capability gained: operators can distinguish a closed boundary loop, a
  simple open chain, and a branched or irregular vertex-connected component
  from stable topology IDs.
- Invariants preserved: source digest and revision binding, bounded inspection,
  read-only behavior, and explicit human review for any later repair.
- API/schema risk: `MeshCleanupIssueReport` gains an additive C++ field; project,
  journal, and patch formats are unchanged.
- Rollback: remove the new report type/field and grouping pass, the Modify
  projection, associated regression assertions, and this continuation.
- Rejected: automatically closing boundaries, treating open edges as defects,
  or splitting point-touching boundaries based on this summary.
- Cross-product trace: Cartographer only. No RegOS, IDA_Training,
  hidden_canopy_bot, or Ask_IDA contract or dataset changed.
- Remaining risk: compilation, regression execution, and native display remain
  unverified; vertex-connected grouping intentionally merges loops that touch
  at a shared vertex and exposes that junction explicitly. Face attribution
  depends on the validated one-face incidence of every boundary edge.

### Exact collinear face-corner discovery (2026-10-26; code-only)

The existing source-bound inspection now reports consecutive face corners whose
positions are exactly collinear under normalized floating-point edge
arithmetic, with the middle corner lying between its neighbors. The Modify
panel shows up to 64 ordered face/vertex triplets. The report is read-only:
collinear corners may carry authored corner-domain values, so the diagnostic
does not imply that a corner can be removed safely. Near-collinear geometry is
not reported.

- Files changed: the application report/API and inspection path, desktop Modify
  projection, this ledger, and the ignored local work queue.
- Capability gained: operators can locate a redundant-looking face corner by
  stable face and vertex IDs without changing the source mesh.
- Invariants preserved: inspection remains bound to the exact source digest,
  project and mesh revisions; work remains within the existing corner budget;
  no mutation authority or project/journal schema changed.
- Tests run: none. No regression test source was added; builds, tests, and
  native UI review were not run under the active code-only instruction.
- Risks: the exact predicate intentionally misses near-collinear cases and has
  no runtime or native-display evidence. The additive C++ report field requires
  downstream source consumers to rebuild.
- Rollback: remove `MeshCleanupCollinearFaceCorner`, its report field and scan,
  the bounded Modify rows, and this ledger/queue entry.
- Rejected: automatic vertex removal, epsilon-based approximate classification,
  and treating authored corner data as disposable.
- Cross-product trace: Cartographer only. No RegOS, IDA_Training,
  hidden_canopy_bot, or Ask_IDA API, dataset, or evaluation contract changed.
  Dataset/training progress is unchanged; capability progress is code-only and
  promotion remains open.

### Duplicate-cycle face inspection wiring (2026-10-26; code-only)

Each displayed face ID in a duplicate-cycle group now has a `Select` control.
It dispatches a one-face `SelectFaceBatchAction` using the report's project
generation, project revision, and mesh revision. Existing application-side
guards reject stale reports or invalid face identities atomically. Selection
only changes the current editor selection; the issue report does not remove a
face or authorize a repair. Any later delete remains an explicit operator
action through the existing face-edit command.

- Files changed: `apps/desktop/main.cpp`, this ledger, and the ignored local
  work queue.
- Capability gained: an operator can move from a reported stable face ID to
  viewport inspection and the existing explicit face-edit controls.
- Invariants preserved: report source binding, stale-generation/revision
  refusal, selection-only behavior, no project/history mutation, and human
  control over any topology edit.
- Tests added/updated/run: none. Existing application source includes
  `application_selects_face_batches_atomically_without_project_mutation`,
  covering the happy path, invalid IDs, stale project and mesh revisions, and
  unchanged project/history state; it was not run in this code-only sprint.
- Tests not run: desktop build, application/UI tests, and native click-through,
  per the instruction to code without testing.
- Risks: compiler and native layout behavior are unverified. Selecting a face
  changes the Modify panel to the face-selection controls; operators can return
  to object selection to inspect another reported ID.
- Rollback: remove the per-face `Select` control and its explanatory ledger
  entry; no schema, patch, or persisted project data changes.
- Rejected: selecting or deleting an entire duplicate group, automatic face
  removal, and bypassing the revision-bound application selection action.
- Cross-product trace: direct impact is Cartographer's desktop editor. No
  indirect contract change; RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA
  were not touched. The existing application selection action is the only
  shared contract used; no API/schema, dataset/eval, deployment, or promotion
  change is claimed.
- Capability Delta: dataset progress unchanged; training progress unchanged;
  capability progress is code-only and unverified; promotion progress remains
  open.
- Required follow-up: compile desktop, run the focused application/UI suites,
  then click through duplicate-face selection and explicit deletion in the
  native editor. Broader mesh cleanup discovery and repair acceptance remain
  open.

### Bounded face-finding inspection wiring (2026-10-26; code-only)

The same revision-bound `SelectFaceBatchAction` now backs selection controls
for face IDs shown by collinear-corner, nonplanar/unmeasurable planarity,
boundary-component, disconnected-fan, and disconnected-face-component
diagnostics. Each control carries a UI-unique identity while selecting exactly
one stable face. The existing per-category display caps remain in force, and
the report labels these controls as transient selection only.

- Files changed: `apps/desktop/main.cpp`, this ledger, and the ignored local
  work queue.
- Capability gained: operators can navigate from additional bounded face
  findings to the matching face in the existing viewport/editor controls.
- Boundary improved: stale report generations/revisions, stale mesh revisions,
  and invalid face IDs remain guarded by the application action; omitted rows
  stay omitted rather than triggering an unbounded GUI expansion.
- Governance preserved: selection changes no project/history state and grants
  no repair authority. Any edit still uses the existing human face command.
- Tests added/updated/run: none. No desktop build, application/UI tests, or
  native interaction was run under the code-only instruction.
- Risks: the new button layout and selection feedback are source-only and
  uncompiled; only the report's existing bounded sample is directly selectable.
- Rollback: remove the report-local selection helper and its calls in the
  face-finding rows, then remove this continuation; no data or schema changes.
- Rejected: direct selection-state writes, changing the diagnostic scan bound,
  expanding every result into an unbounded native control list, or applying a
  topology repair from a finding.
- Cross-product trace: Cartographer desktop only. No shared contract, API,
  schema, dataset/evaluation, deployment, or other product changed.
- Capability delta: dataset/training progress unchanged; capability progress
  is code-only and unverified; promotion remains open.
- Required follow-up: compile the desktop target, exercise each face-finding
  selection route, verify stale-report refusal, and visually review the
  bounded row layout. Broader issue discovery and repair acceptance remain
  open.
