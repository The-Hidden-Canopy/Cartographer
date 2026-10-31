# Project diff and merge review

## Current source review

`agent::ProjectDiff::between` binds both complete `ProjectDocument` snapshots
with their canonical serialization digests and revisions. `has_changes()`
checks those digests in addition to the detailed object, mesh, material,
texture, Engineering, and CONDITION rows. A change to another authored source
domain therefore cannot be reported as an empty project diff.

The Agent Workbench shows both source revisions and digest prefixes. It labels
the detailed rows as a partial projection for every changed snapshot. The full
digests remain the identity of the reviewed snapshots; they are not a
substitute for decoding the changed values or proof that a particular omitted
domain changed.

Checkpoint rebase remains conservative: object, mesh, and material-region
read/write conflicts block safe rebase, and the speculative result stays
separate from the live project until explicit acceptance. Existing rebase
evidence covers source advancement, invalidated steps, and disjoint writes.

## Acceptance evidence

- `project_diff_marks_changed_snapshots_as_partial_projections` combines a
  journaled spatial-field change with a categorized object change, then
  verifies the full digest and partial-projection signal. It also verifies
  that unchanged snapshots are not marked partial and that a presentation
  catalog change is marked partial even though it has a bounded category row.
- `cartographer_agent_workbench_tests` built successfully and CTest passed
  1/1 on 2026-10-20.
- Existing geometry, UV, material, and texture diff cases remain in the same
  test executable and passed in that run.

## CONDITION state-branch resolution slice (2026-10-20)

This adds an explicit three-way resolution path for authored CONDITION state
branches. Each difference carries the common-ancestor, left, and right values,
and all three values participate in the comparison digest. Identical changes on
both sides carry through; each differing value requires an explicit choice of
common ancestor, left, or right. Resolution creates a child branch whose parent
is the common ancestor; it does not overwrite the source authoring.

The resolver recomputes the comparison from current authoring and rejects a
changed revision, authoring digest, common ancestor, comparison digest, invalid
choice, duplicate or missing choice, or a caller-modified comparison. The
CONDITION UI shows each value and whether a difference is contested or one
sided, requires a choice for every difference, and submits the resolved branch
through the existing UI provider dispatch and project transaction path. Stale
resolution is refused without advancing project revision. The resulting branch
survives save/reopen and undo/redo.

Evidence on 2026-10-20:

- `cartographer_condition_authoring_tests` and `cartographer_ui_tests` rebuilt
  successfully (174 Ninja steps) and the focused CTest selection passed 2/2
  after the resolver was hardened to use only a freshly recomputed comparison.
  The pure authoring case covers contested and disjoint choices, missing and
  invalid choices, shared identical changes, stale authoring, and
  caller-modified comparison data.
- `ui_safe_place_branch_is_journaled_comparable_and_recoverable` covers explicit
  commit through the UI controller, stale refusal without revision change,
  save/reopen digest parity, and undo/redo.
- The Windows Vulkan `cartographer_desktop` target built and linked with this
  UI slice. That build preceded the resolver-only hardening; the current
  headless build and authoring tests cover the hardened resolver. Native
  rendering and click-through were not exercised.

This is bounded evidence for CONDITION state branches. The whole-snapshot
Agent Workbench follow-up below adds a generic base/current/speculative
comparison and explicit source selection. Arbitrary authored domains still
lack field-level conflict rows and independent resolution choices, and no
native interaction capture proves the desktop controls visually.

## Whole-snapshot Agent Workbench integration (2026-10-25)

The Agent Workbench now loads the task base, live project, and speculative
snapshot into a digest-bound comparison. It shows the current and speculative
deltas from the common snapshot, identifies contested whole-project state,
and requires an explicit common/current/speculative choice for contested
snapshots. Selection prepares an in-memory complete-snapshot candidate; it does
not combine fields or authored domains.

Human acceptance rechecks the exact review and candidate, the live revision,
and all required Agent gates. It requires a printable reason and replaces the
complete selected snapshot through `ReplaceProjectDocumentCommand`, command
history, and the existing application mutation path, which appends to the
project journal when one is configured. It rejects stale reviews and candidates
that already match the live project. The operation includes the task, selected
source, review and candidate digests, and reason.

Code was added for side-by-side summaries, explicit resolution, and acceptance.
The adversarial acceptance test was authored but not run at the user's request.
No build or test was run for this integration. Native click-through and
screenshot acceptance remain unverified.

Field-level conflict rows and independent choices for arbitrary authored
domains remain incomplete. Whole-snapshot choices can discard the changes on
the unselected side, and the UI warns about that before acceptance. This
integration therefore does not complete the broader merge queue item.

## Bounded merge detail projection (2026-10-25, code-only)

The merge comparison now carries sorted, read-only detail rows for supported
object, mesh, material binding/source, texture source/blob, variant, sampler,
graph, and graph-binding changes. Rows identify the record and summarize the
changed fields or bounded mesh counts on the current and speculative sides.
Where `ProjectDiff` retains before/after data, rows also show available names,
IDs, revisions, digest prefixes, mesh extents, and sidecar byte counts. Each
side contributes at most 64 records; the view reports truncation. Opaque
sidecars remain digest-bound, and every changed snapshot is marked as a partial
projection without claiming that a specific omitted domain changed. The
desktop table states that source choices still select complete snapshots. No
per-field candidate or mutation path was added.

The UI regression now asserts that current-only and speculative-only object
transform changes appear on their correct sides and that details remain
available after preparing a candidate. It was authored but not run, per the
user's code-only instruction. No build, test, audit, or native interaction
check was run for this slice.

## Object value detail continuation (2026-10-25, code-only)

The comparison projection now retains before/after object names, local
transforms, parent links, mesh bindings, visibility, and lock state. The desktop
detail row shows those values for changed fields and all available values for
added or removed objects. Transform numbers use locale-independent,
round-trip formatting so small authored differences are not rounded away in
review text. This remains a read-only detail projection; the accepted result is
still an exact whole-snapshot choice.

The existing merge acceptance regression now asserts exact transform values
on the current-only and speculative-only rows, in addition to checking side
attribution. The assertion was authored but not run under the code-only sprint
constraint. No build, test, or native interaction check was run after this
continuation. A separate read-only public-boundary audit passed for the current
tree and selected refs `HEAD` and `origin/main`; it does not compile or verify
the merge-review code.

## Material catalog value projection continuation (2026-10-25, code-only)

The same read-only comparison rows now show retained sampler U/V/W addressing
and minification/magnification/mip filters, material-variant override and mesh
scope counts with revisions, material-graph revisions, and graph-binding
source revisions and digest prefixes. Unknown enum values render as invalid
labels rather than being mapped to a valid mode. The graph and sampler data
remain diagnostic values; the resolver still chooses one complete snapshot.

The existing diff regression now asserts that the sampler change retains its
exact before/after address mode. It was authored but not run. The UI formatting
and desktop rendering remain unverified; no build or test was run for this
continuation. The post-edit read-only public-boundary audit returned
`PUBLIC_BOUNDARY_PASS` for the current tree and selected refs `HEAD` and
`origin/main`.

## Bounded mesh value detail continuation (2026-10-25, code-only)

The review now adds separate read-only rows for sampled vertex position
transitions, face vertex lists, attribute-layer counts, UV-set summaries, and
sampled UV-corner transitions. It shows at most eight rows per mesh value kind
and twelve vertex IDs per face row, in addition to the existing per-side
64-record cap. The UI labels both per-mesh sample truncation and overall row
truncation. When a UV set lacks detailed corner samples, the row says so rather
than implying the values were compared in the UI.

An Agent Workbench regression now creates one speculative vertex move and
asserts that its exact before/after detail appears only on the speculative side
without changing the live project. The test was authored but not run. Existing
diff coverage also checks exact UV-corner values in the source diff; it does not
verify their rendered UI rows. No build, test, or native interaction was run
for this continuation.

## Mesh descriptor detail continuation (2026-10-25, code-only)

Attribute-layer diff records now retain optional before/after descriptors.
The merge projection displays the domain and value type transition for added,
removed, or retyped layers. UV rows now display bounded before/after set
descriptors, including the name, bound corner layer, editing/render roles,
lightmap-candidate flag, and tile-policy version. Long descriptor labels are
clipped at a UTF-8 boundary and report the omitted byte count. The existing
per-side and per-mesh row caps still apply.

The source-diff regression now covers an attribute type change and a UV-set
rename while preserving exact before/after descriptors. It was authored but
not run. No build, test, or native interaction was run for this continuation.
It does not add arbitrary authored-domain values or per-field merge choices.
The post-edit read-only public-boundary audit returned
`PUBLIC_BOUNDARY_PASS` for selected refs `HEAD` and `origin/main`; this does not
verify C++ behavior or audit other public refs.

## Material definition detail continuation (2026-10-25, code-only)

Material source diffs now retain their before/after `StandardMaterial`
definitions. The review reports only differing scalar surface fields and
texture slots, including texture asset, UV-set and sampler identities,
transform, channel mapping, interpretation, and normal convention. Values use
locale-independent round-trip number formatting. Added or removed materials
show non-default definition values without expanding unchanged defaults into
noise.

The source-diff regression now asserts exact roughness and texture-binding
values across a changed material source. It was authored but not run. The
projection remains diagnostic: it does not introduce per-field choices or
merge authority. No build, test, or native interaction was run for this slice.
The post-edit read-only boundary audit returned `PUBLIC_BOUNDARY_PASS` for
selected refs `HEAD` and `origin/main`; it does not establish C++ behavior or
clear other public refs.

### Material detail cross-product trace

- **Direct impact:** Cartographer's read-only merge review exposes retained
  `StandardMaterial` transitions, including PBR values and texture bindings.
- **Indirect impact:** `MaterialSourceChange` is source-level additive and adds
  fixed-size material values to in-process diffs; there is no persisted format
  change.
- **Shared contract:** no RegOS permission/audit, IDA_Training dataset/eval,
  hidden_canopy_bot API, or Ask_IDA query contract changed.
- **Risk:** UI and C++ behavior remain uncompiled; no deployment was performed.
- **Required follow-up:** run Agent Workbench and UI coverage, build the desktop,
  and inspect material and texture transitions in the native merge panel.

## Material-graph node detail continuation (2026-10-26, code-only)

Material-graph diffs now retain before/after output bindings and stable-ID node
samples. The sample is deterministic and capped at 32 changed nodes per graph;
each row exposes node kind/type, ordered input IDs, and the active payload for
constants, parameters, texture samples, surface inputs, or unsupported nodes.
The graph row exposes changed output-port bindings. Per-side warnings identify
when a graph exceeded the node sample cap, while the existing 64-record side
cap remains in force.

The Agent Workbench regression now checks exact node constants and output
bindings for an edited graph, plus deterministic sampling and truncation for a
40-node addition. It was authored but not run. No build, test, or native panel
inspection was performed. This remains diagnostic and does not provide graph
field-resolution or mutation authority. The post-edit read-only public-boundary
audit returned `PUBLIC_BOUNDARY_PASS` for selected refs `HEAD` and `origin/main`;
it does not verify C++ behavior or clear other public refs.

### Material-graph detail cross-product trace

- **Direct impact:** Cartographer merge review exposes changed graph nodes and
  output bindings from the retained source snapshots.
- **Indirect impact:** `MaterialGraphChange` gains source-level optional values
  and a bounded node sample; project/package serialization is unchanged.
- **Shared contract:** no RegOS, IDA_Training, hidden_canopy_bot, or Ask_IDA
  contract changed; no dataset or evaluation artifact changed.
- **Risk:** UI and C++ changes remain uncompiled; no deployment was performed.
- **Required follow-up:** run the Agent Workbench tests and UI coverage, build
  desktop, and inspect current/speculative graph rows in the native merge panel.

### Bounded detail projection repair ledger

- Files changed for this follow-up: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/src/ui.cpp`,
  `libs/ui/include/carto/ui/ui.hpp`, `apps/desktop/main.cpp`,
  `tests/agent_workbench_tests.cpp`, this file, and
  `docs/CARTOGRAPHER_WORK_QUEUE.local.md`.
- Capability gained: operators can inspect supported object, catalog, and
  bounded mesh and material-graph value transitions on both sides of a
  contested comparison before choosing a complete snapshot.
- Boundary improved: per-side 64-record, per-mesh, and per-graph sample caps
  are visible; partial projection is labeled for changed snapshots without
  inferring that an omitted domain changed; diagnostic rows do not create merge
  authority.
- Governance preserved: comparisons are read-only, stale/candidate checks and
  human acceptance gates remain in place, and acceptance still uses the
  existing project replacement/history/journal path.
- Tests added: side attribution and exact before/after transform values for
  current-only and speculative-only object changes, plus detail retention after
  candidate preparation; retained sampler address values in the diff; and
  speculative mesh-value row attribution without source mutation; before/after
  attribute descriptor values and UV descriptor rename coverage; before/after
  PBR and texture-binding values for changed material sources; graph node and
  output retention plus deterministic 40-node truncation coverage; and exact
  material-variant override and permitted-mesh sample retention.
- Tests run: none, per the user's instruction.
- Tests not run: all builds, tests, and native interaction checks.
- Risks: object, catalog, mesh, descriptor, material, graph, and variant value
  formatting is uncompiled; other authored records expose only values retained by their
  existing `ProjectDiff` rows; categories without detailed records remain
  opaque; no independent field resolution exists.
- Rollback path: revert only this slice's changes to the UI view model,
  comparison projection, desktop table, regression assertions, and these two
  documentation sections. No schema or saved project data changed.
- Intentionally rejected: auto-combining disjoint fields, implying that detail
  rows represent every authored domain, or treating a diagnostic row as
  permission to mutate.

## Texture-source detail continuation (2026-10-26, code-only)

Texture-source diffs now retain complete optional before/after records. Merge
review rows show changed names, project-relative locators, digest prefixes,
formats, dimensions, provenance, and source revisions; added and removed
records show all available values. Locator, name, and provenance labels use a
bounded projection that avoids splitting a valid multi-byte UTF-8 sequence.
The full source record remains in memory for
comparison, while the serialized project format and snapshot-selection path
are unchanged.

The source-diff regression now checks changed locator, digest, format,
dimensions, provenance, and revision values, plus the retained record for an
added texture. It is authored but not run. No build, test, or native panel
inspection was performed. The read-only selected-ref public-boundary audit
returned `PUBLIC_BOUNDARY_PASS` for `HEAD` and `origin/main`; texture field
rendering and C++ behavior remain unverified, and other public refs are outside
this audit.

### Texture-source detail cross-product trace

- **Direct impact:** Cartographer merge review can distinguish texture content
  changes from metadata changes and show the source record values on each side.
- **Indirect impact:** `TextureSourceChange` gains in-memory optional source
  records; no serialized schema, external API, or provider path changes.
- **Shared contract:** no RegOS, IDA_Training, hidden_canopy_bot, or Ask_IDA
  contract changed; no dataset or evaluation artifact changed.
- **Risk:** UI/C++ changes remain uncompiled; long labels are clipped with an
  explicit omitted-byte count, and digests remain abbreviated in the row.
- **Required follow-up:** run Agent Workbench/UI coverage, build desktop, and
  inspect added, removed, and metadata-edited textures in the native merge UI.

### Texture-source detail repair ledger

- **Files changed:** `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/src/ui.cpp`,
  `tests/agent_workbench_tests.cpp`, this file, and
  `docs/CARTOGRAPHER_WORK_QUEUE.local.md`.
- **Capability gained:** source rows can present metadata transitions retained
  by the existing texture comparison, including added and removed values.
- **Invariants preserved:** read-only comparison, full-project source digests,
  whole-snapshot human selection, existing schema bytes, and no new mutation
  authority.
- **Tests added:** before/after content, locator, format, dimensions,
  provenance, and revision assertions; added-record retention assertions.
- **Tests run:** none, per the user's code-only instruction.
- **Tests not run:** all builds, tests, and native interaction checks.
- **Risks:** current C++ and UI formatting remain uncompiled; label clipping
  omits long string tails while reporting the omitted byte count.
- **Rollback path:** revert the texture source snapshots, row formatting,
  regression assertions, and this continuation's documentation only. No project
  schema or saved data changed.
- **Intentionally rejected:** exposing arbitrary source fields as mutation
  controls or converting these values into independent merge choices.

## Repair ledger

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `apps/desktop/main.cpp`,
  `tests/agent_workbench_tests.cpp`.
- Capability gained: full-document differences cannot be mistaken for an
  empty diff; reviewed snapshot identities are visible on both sides.
- Invariants preserved: source revision/digest binding, conservative rebase,
  speculative/live separation, and explicit human acceptance.
- Tests run: `cartographer_agent_workbench_tests` build; CTest 1/1 passed.
- Tests not run at that time: desktop native-window rendering and generic
  three-way merge acceptance. The later whole-snapshot integration below adds
  the code path; it remains unbuilt and untested.
- Risks: unmodeled values remain opaque in detailed review, especially when
  known and unmodeled domains change together.
- Rollback path: revert only the four listed code/test changes; no project
  schema or saved project data changed.
- Intentionally rejected: treating matching or disjoint whole-document
  digests as a per-record merge, or auto-selecting a side for contested data.

### CONDITION state-branch slice repair ledger

- Files changed: `libs/condition_authoring/include/carto/condition/state_branch.hpp`,
  `libs/condition_authoring/src/state_branch.cpp`, `libs/ui/include/carto/ui/ui.hpp`,
  `libs/ui/src/ui.cpp`, `apps/desktop/main.cpp`,
  `tests/condition_authoring_tests.cpp`, and this verification note.
- Capability gained: authored state branches can be compared and resolved
  into a common-ancestor child with explicit per-value human choices.
- Boundary improved: stale, incomplete, duplicate, invalid, or caller-altered
  comparisons cannot silently select or commit contested values.
- Governance preserved: source authoring stays unchanged; current revision and
  digest are rechecked; the result uses the existing project transaction,
  journal, and undo/redo path; no provider execution or private runtime is
  involved.
- Tests run: the headless build completed 174 steps; focused CTest passed 2/2
  for `cartographer_condition_authoring_tests` and `cartographer_ui_tests`.
  The tests include the adversarial cases listed above and UI persistence/undo
  coverage. The build emitted 13 missing-field-initializer warnings in
  `libs/application/src/application.cpp`; no new resolver-specific warning was
  reported.
- Tests not run: native desktop interaction/capture and general project-wide
  merge acceptance; that workflow remains incomplete.
- Risks: this only resolves CONDITION authored state branches; other authored
  domains remain opaque or unmergeable.
- Rollback path: revert the six code/UI/test files listed above; remove this
  slice's evidence section to restore the prior verification note. No project
  schema or saved project data changed.
- Intentionally rejected: automatic resolution of contested values and any
  claim that this slice completes arbitrary project merge.

### Whole-snapshot Agent Workbench wiring ledger

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`,
  `libs/application/include/carto/application/application.hpp`,
  `libs/application/src/application.cpp`, `libs/ui/include/carto/ui/ui.hpp`,
  `libs/ui/src/ui.cpp`, `apps/desktop/main.cpp`,
  `tests/agent_workbench_tests.cpp`, this file, and
  `docs/CARTOGRAPHER_WORK_QUEUE.local.md`.
- Capability gained: a stale Agent branch can be compared against the live
  project, reviewed as a complete source snapshot, and accepted through the
  existing human mutation path, with journaling when configured.
- Boundary improved: contested snapshots require a source choice and reason;
  stale revisions, failed required gates, blank reasons, and no-op replacement
  are refused.
- Governance preserved: acceptance passes through `HumanApplicationAccess`,
  `ReplaceProjectDocumentCommand`, command history, and the application
  mutation/journal path. No direct project write or automatic field merge was
  added.
- Tests added: stale and no-op acceptance refusal, side-by-side source deltas,
  explicit choice, reason requirement, human operation lineage, journal event
  count, and undo/redo.
- Tests run: none, per the user's code-only instruction.
- Tests not run: all builds, test suites, and native interaction checks.
- Risks: full-snapshot selection can discard the unselected side; authored
  domains without detailed diff rows remain opaque behind the full-project
  digests; the new C++ changes are uncompiled and unverified.
- Rollback path: revert only the whole-snapshot wiring changes listed above;
  no schema or saved project data changed.
- Intentionally rejected: merging arbitrary fields without domain-specific
  semantics, accepting stale snapshots, or marking the broader queue item
  complete without verification and native acceptance.

### Subsequent executed whole-snapshot verification

The later desktop/authoring continuation rebuilt and passed
cartographer_agent_workbench_tests together with fourteen focused suites
(15/15, 61.39 seconds). The whole-snapshot acceptance regression described
above executed successfully, including explicit choice/reason, stale/no-op
refusal, human lineage, journal count, and undo/redo. Native arbitrary-domain
merge interaction and field-level resolution remain open; this closes the
earlier compile/test gap only. See
[source history acceptance](source-history-acceptance.md).

## Material-variant detail continuation (2026-10-26, code-only)

Variant diffs now retain both complete optional override records and bounded
samples of permitted mesh asset IDs. The UI projection renders every changed
PBR override and texture binding as a before/after transition, distinguishes an
absent variant from an unset override, shows unrestricted or sampled mesh
scope, and warns when more than 16 permitted mesh IDs are omitted for either
review side. The detail row is appended after all transitions have been
collected, so the bounded scope value remains visible with the other fields.
This stays read-only; resolution still selects a complete project snapshot.

The source-diff regression now checks the retained roughness override and exact
permitted mesh sample. The regression is authored but not run. No build, test,
or native panel inspection was performed. C++/UI behavior therefore remains
unverified, and broad authored-domain merge plus independent field choices
remain incomplete. After the source/UI/docs changes, the read-only selected-ref
audit returned `PUBLIC_BOUNDARY_PASS` for `HEAD` and `origin/main`; it does not
verify C++ behavior or inspect other public refs.

### Material-variant detail cross-product trace

- **Direct impact:** Cartographer review rows can expose exact changed variant
  override values and a bounded sample of the mesh assets they permit.
- **Indirect impact:** `MaterialVariantChange` gains in-memory before/after
  values and bounded samples; the project schema and serialized format are
  unchanged.
- **Shared contract:** no RegOS, IDA_Training, hidden_canopy_bot, or Ask_IDA
  contract changed; no dataset or evaluation artifact changed.
- **Risk:** the C++ and desktop rendering changes remain uncompiled; values
  beyond the stated sample cap are omitted and signaled.
- **Required follow-up:** run Agent Workbench/UI coverage, build desktop, and
  inspect a scoped material-variant transition in the native merge panel.

### Transform formatter compile integration (2026-10-26)

The current-source build exposed a callback type mismatch: object detail rows
passed an `ObjectStateDetail` directly to the `Transform` formatter. The narrow
repair extracts `local_transform` through an object formatter callback for
before and after values. This changes no comparison, resolution, or publication
authority. Existing Agent Workbench tests assert exact transform transitions
and that comparison leaves the project unchanged. The final complete
`cartographer_agent_workbench_tests` passed (1/1, 5.86 s total), the public UI
suite passed, and the desktop linked. Native panel inspection remains unrun.

The selected Agent Workbench build also exposed four fixture assignments to
nonassignable project-load results. Separate immutable load results now retain
the existing initial/undo/redo coordinate checks. No public project assignment
or mutation authority was added to make a test compile.

Rollback is the callback and its two call sites in `libs/ui/src/ui.cpp`; retain
the surrounding shared-workspace review implementation. No schema, migration,
dataset, remote service, or cross-product contract is changed by this repair.

## Presentation-catalog attribution continuation (2026-10-26, code-only)

`ProjectDiff` now separately records whether the authored presentation catalog
changed. The comparison table attributes that change to a bounded category row,
and the desktop identifies it in the current/speculative and candidate
summaries. It states that record-level camera, light, and reference-plane
details are unavailable; snapshot selection remains whole-project only. The
catalog continues to participate in the complete project digest.

An unrun regression authors a camera through `ProjectTransaction` and checks
that a presentation-only change is visible and marks the whole snapshot detail
projection as partial. No build, test, or native interaction was run, per the
user's code-only instruction.

### Presentation-catalog attribution ledger

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/src/ui.cpp`,
  `apps/desktop/main.cpp`, `tests/agent_workbench_tests.cpp`, this file, and
  `docs/CARTOGRAPHER_WORK_QUEUE.local.md`.
- Capability gained: authored camera/light/reference-plane catalog changes
  are attributed by category in project diffs and review summaries.
- Invariants preserved: complete snapshot digests remain authoritative;
  comparison remains read-only; merge choices still select a complete snapshot;
  camera/light/reference-plane field details and field-level merge authority
  are not implied.
- Tests added: journaled presentation-only source change checks category
  detection and classification.
- Tests run: none; no build or test was run at the user's request.
- Tests not run: all compilation, automated suites, desktop rendering, and
  native interaction acceptance.
- Public/private boundary audit: `PUBLIC_BOUNDARY_PASS` for the current tree
  and selected history refs `HEAD` and `origin/main`; other refs were not
  included. This is publication-boundary evidence, not code validation.
- Risks: the C++ changes are uncompiled; serialization comparison cost and
  behavior remain unmeasured. Omitted domains are not decoded or classified;
  the UI reports partial projection and binds the complete snapshot by digest.
- Rollback path: remove the presentation flag and its digest comparison,
  category row and desktop labels, and this regression; remove this section
  and its local queue continuation. No schema or project data changed.
- Intentionally rejected: decoding presentation records or adding field-level
  merge choices without bounded record semantics and separate acceptance
  evidence.

## Presentation-record detail continuation (2026-10-26, code-only)

The category-only presentation row now has bounded record-level attribution.
`ProjectDiff` compares persisted camera, light, and reference-plane records by
all current authored fields, retains each changed record's complete before and
after values plus revisions, and counts every changed record. Samples are
deterministic and capped at 64 records per diff; counts and a truncation flag
preserve visibility into changes beyond that cap. The UI renders these values
in the current/speculative detail rows and exposes category counts and sample
status in candidate summaries.

The details remain a read-only projection. The complete project digest still
binds the selected snapshot, the UI still warns that category coverage is
partial, and merge acceptance still selects one complete snapshot through the
existing history and journal path. No field-level resolution, new authority,
project serialization, or cross-product contract was added.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/src/ui.cpp`,
  `libs/ui/include/carto/ui/ui.hpp`, `apps/desktop/main.cpp`,
  `tests/agent_workbench_tests.cpp`, and this file plus the local work queue.
- Capability gained: reviewers can identify changed presentation records and
  inspect their authored before/after values without treating a category flag
  as record-level evidence.
- Boundary improved: camera, light, and reference-plane additions, removals,
  and field changes are attributed; samples remain bounded and truncation is
  explicit.
- Governance preserved: complete-snapshot digest binding and human-selected
  whole-snapshot acceptance remain unchanged; no presentation field becomes
  independently mergeable.
- Tests added: all three record kinds are represented in an authored diff;
  sample counts remain accurate after the 64-record detail cap; existing UI
  merge coverage checks presentation category and camera detail rows.
- Tests run: none, at the user's request for code-only work.
- Tests not run: compilation, agent workbench/UI suites, desktop rendering,
  cross-product suites, and native interaction acceptance.
- Public/private boundary: a post-change read-only audit returned
  `PUBLIC_BOUNDARY_PASS` for the current tree and selected refs `HEAD` and
  `origin/main`. Other refs and the optional ignore-policy probe were not
  included. The new projection contains authored project presentation values
  and project-relative source locators, not source bytes or credentials.
- API/schema risk: public C++ diff structs now retain bounded records, so
  downstream binary consumers need a rebuild. No serialized project schema
  changed.
- Dataset/eval risk: no dataset, labels, prompts, or evaluation expectations
  changed. Existing eval suites still need a run before promotion.
- Deployment risk: no deployment or cross-product behavior was verified; the
  uncompiled UI and diff changes must pass their target builds before release.
- Rollback path: remove `PresentationRecordChange` and the new `ProjectDiff`
  fields and collection logic, remove the presentation row formatting and
  summary labels, revert the added regression, and remove this continuation.
  The earlier category-only attribution and partial-projection warning can
  remain.
- Intentionally rejected: unbounded detail retention, decoding unrelated
  opaque domains, and independent presentation-field merge choices.
- Remaining work: broader authored-domain diffs, independent field resolution,
  compilation, automated suites, and native review remain open. The merge
  work-queue item stays started.

## Spatial-field detail continuation (2026-10-26, code-only)

Project diffs now attribute scalar spatial-field additions, removals, metadata,
revision, digest, and bounded sample changes. Each side reports the semantic,
grid extent/origin/cell size, declared value range, revision, canonical digest,
and sample count. Up to 64 changed samples per field are shown with grid
coordinates and before/after values; exact changed counts remain available
when the sample list is truncated. A changed grid origin, spacing, or shape
suppresses cell-to-cell comparisons and is called out explicitly rather than
presenting equal array indices as the same spatial location.

These rows remain diagnostic. Full-project digests still bind complete
snapshots, partial-projection warnings remain active, and merge acceptance
still selects one complete snapshot. No field-level merge choice, project
schema, or other product contract changed.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/include/carto/ui/ui.hpp`,
  `libs/ui/src/ui.cpp`, `apps/desktop/main.cpp`,
  `tests/agent_workbench_tests.cpp`, this file, and the local work queue.
- Capability gained: reviewers can inspect bounded scalar-field source facts
  and exact changed sample values from the current and speculative snapshots.
- Boundary improved: add/remove, value edits, grid relocation, and over-cap
  samples are distinguishable; grid changes cannot create false sample
  correspondence.
- Governance preserved: no data mutation or automatic field composition is
  added; snapshot digest binding and human whole-snapshot selection remain the
  only merge authority.
- Tests added: field addition and source summary, a one-cell value edit,
  relocated-grid refusal of sample correspondence, a 256-sample field capped
  at 64 entries, and UI merge-summary/detail projection.
- Tests run: none, at the user's request for code-only work.
- Tests not run: compilation, Agent Workbench/UI suites, desktop rendering,
  cross-product suites, and native interaction acceptance.
- Public/private boundary: the read-only audit returned
  `PUBLIC_BOUNDARY_PASS` for the implementation tree and selected refs `HEAD`
  and `origin/main`; broader refs and the optional ignore-policy probe were
  not included.
- API/schema risk: public C++ diff and UI summary structs gain trailing detail
  fields, so binary consumers need a rebuild. No serialized schema changed.
- Dataset/eval risk: no dataset, labels, prompts, or eval expectations changed.
- Deployment risk: builds and native review remain open; this code is not
  release-verified.
- Rollback path: selectively remove the scalar-field summary/sample structs,
  diff collection/count and merge UI wiring, the added test cases, and this
  continuation. Preserve existing snapshot digest and partial-projection
  behavior.
- Intentionally rejected: copying complete field arrays into every diff,
  comparing coordinates across changed grids, decoding opaque semantic
  sidecars, or allowing independent field-value composition.
- Remaining work: other authored project domains, native review, compilation,
  automated acceptance, and independent merge resolution remain open. The
  source-aware merge queue item stays started.

## Partial-projection warning integration (2026-10-26, code-only)

`ProjectDiff` now reports that the source projection is partial for every
changed full-project digest, including when detailed object/catalog rows are
also present. The old “unclassified-only” signal suppressed this warning for
mixed changes. UI summaries and detail rows now state that category coverage is
partial and the full-project digest binds the snapshot; they do not claim that
an omitted domain definitely changed. Snapshot selection and acceptance remain
whole-project operations.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/include/carto/ui/ui.hpp`,
  `libs/ui/src/ui.cpp`, `apps/desktop/main.cpp`,
  `tests/agent_workbench_tests.cpp`, this file, and
  `docs/CARTOGRAPHER_WORK_QUEUE.local.md`.
- Capability gained: a mixed recognized/opaque change cannot suppress the
  partial-projection warning or be mislabeled as a fully detailed diff.
- Invariants preserved: full snapshot digests remain authoritative; diffing is
  read-only; merge choices still select complete snapshots through existing
  human acceptance and journaling.
- Regression authored: a spatial-field change combined with an object change,
  a presentation-catalog-only change, and an unchanged snapshot.
- Compatibility: the legacy `has_unclassified_source_changes` method and UI
  fields retain their previous digest-only semantics; the new partial-projection
  signal is additive.
- Tests run: none. The user requested coding without tests; no build, test,
  lint, or native interaction was performed.
- Public/private audit: `PUBLIC_BOUNDARY_PASS` for the current tree and selected
  refs `HEAD` and `origin/main`. The optional `--check-ignore-policy` probe
  could not write its temporary `.gitignore` under the Windows sandbox; the
  boundary scan was rerun without that probe.
- Risks: C++ compile and UI rendering remain unverified. The new partial
  projection method and summary fields are additive; legacy unclassified
  methods/fields retain their previous digest-only semantics. Public summary
  structs gain trailing fields, so external binary consumers must rebuild. The
  warning indicates partial category coverage, not that a particular omitted
  domain changed.
- Rollback: restore the prior indicator/summary field names and wording, test
  assertions, and this ledger. No serialized project data or schema changed.
- Intentionally rejected: fabricating a residual-domain detector from the
  complete digest or asserting that omitted authored records changed.
- Shared contract touched: Cartographer's public C++ diff/UI summary contract
  gains additive fields/methods; no project serialization or cross-repository
  schema changed.
- Cross-product trace: Cartographer only. No RegOS, IDA_Training,
  hidden_canopy_bot, or Ask_IDA contract, dataset/evaluation, or deployment
  state changed; dataset and training progress remain unchanged, and promotion
  remains open.

## Source-asset registry detail continuation (2026-10-26, code-only)

Project diffs now attribute source-asset registry changes by record identity,
compare complete serialized records for exact change detection, and retain at
most 64 before/after record summaries. Each summary exposes the project-relative
source path, generation, kind, source and artifact digests, importer/options,
domain digests, bounded tag/dependency samples and counts, and a digest of the
complete serialized record. Botanical receipt display is digest-only: the
private source snapshot is not copied into the projection. The UI shows exact
changed-record counts and indicates when registry details are unavailable or
the sample is truncated. Candidate summaries expose the selected source's
record count.

These are read-only details. The complete project digest remains authoritative,
partial-projection warnings remain active, and human acceptance still selects
one complete snapshot. No project serialization, source registry schema, or
cross-product contract changed.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/include/carto/ui/ui.hpp`,
  `libs/ui/src/ui.cpp`, `apps/desktop/main.cpp`,
  `tests/agent_workbench_tests.cpp`, this file, and the local work queue.
- Capability gained: reviewers can inspect source-registry changes alongside
  other bounded project details and see whether those records belong to the
  current or speculative snapshot.
- Boundary improved: record additions, removals, metadata changes, registry
  digest changes without active-record detail, and over-cap samples are
  separately represented; sensitive botanical snapshot bytes are excluded.
- Governance preserved: diffing does not mutate project state or widen merge
  authority; acceptance remains digest-bound, human-selected, journaled, and
  undoable through the existing path.
- Tests added: source-record add, metadata edit, 65-record cap, and UI current
  versus speculative summary/detail assertions including candidate projection.
- Tests run: none, at the user's request for coding without testing.
- Tests not run: compilation, Agent Workbench/UI suites, desktop rendering,
  cross-product suites, and native interaction acceptance.
- Public/private boundary: the read-only scan returned
  `PUBLIC_BOUNDARY_PASS` for the current tree and selected refs `HEAD` and
  `origin/main`; it does not check other refs or establish release readiness.
- API/schema risk: public C++ diff and UI summary structs gain trailing fields,
  so downstream binary consumers need a rebuild. No serialized project or
  source-registry schema changed.
- Dataset/eval risk: no dataset, labels, prompts, or evaluation expectations
  changed; existing evaluations remain unrun.
- Deployment risk: uncompiled code and native display behavior remain
  unverified; release confidence is unchanged.
- Rollback path: remove the source-record summary/change structures and
  collection/count logic, source-registry UI rows and candidate count, the
  focused regression assertions, and this continuation. Keep complete-snapshot
  digest binding and the partial-projection warning.
- Intentionally rejected: retaining the full serialized source record in the
  UI, exposing botanical source snapshots, unbounded record/tag/dependency
  lists, or adding independent source-record merge choices.
- Cross-product trace: Cartographer only. RegOS, IDA_Training,
  hidden_canopy_bot, and Ask_IDA contracts were not changed. Dataset and
  training progress remain unchanged; capability evidence is code-only, and
  promotion remains open.

## Connection-port graph detail continuation (2026-10-26, code-only)

The project merge diff now compares the authored connection-port graph by its
canonical digest and attributes changed port and edge records. It retains at
most 64 summaries of each kind, with exact changed counts and explicit
truncation. Port summaries include owner, local frame and transform,
coordinate convention, and bounded compatibility/capability samples. Edge
summaries include endpoint identities, connection kind, and bounded captured
compatibility evidence. A graph digest change caused by history or another
graph-level value without a changed active port/edge remains visible as a
category change with record details marked unavailable.

The UI labels the edge evidence as structural compatibility evidence; it does
not imply reference behavior, native evidence, or full-game acceptance. The
diff retains at most 64 records of each kind, while the merge detail view shows
at most four port and four edge records per side and reports truncation. The
diff remains read-only and acceptance still chooses one complete snapshot.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/include/carto/ui/ui.hpp`,
  `libs/ui/src/ui.cpp`, `apps/desktop/main.cpp`,
  `tests/agent_workbench_tests.cpp`, this file, and the local work queue.
- Capability gained: reviewers can inspect changed authored ports and typed
  edges on the current or speculative side before selecting a complete source
  snapshot.
- Boundary improved: port/edge additions, removals, metadata edits, graph-only
  digest changes, and over-cap samples remain distinguishable without
  promoting structural evidence to runtime or game acceptance.
- Governance preserved: the comparison is read-only; no edge is created or
  accepted through review, and existing human whole-snapshot acceptance,
  journaling, and undo remain authoritative.
- Tests added: current-side graph addition with 130 ports and 65 edges,
  category and per-record detail assertions, 64-record projection caps,
  four-port/four-edge UI row caps, explicit truncation, current/speculative
  attribution, and candidate summary counts.
- Tests run: none, at the user's request for coding without testing.
- Tests not run: compilation, Agent Workbench/UI suites, desktop rendering,
  cross-product suites, and native interaction acceptance.
- Public/private boundary: the read-only scan returned
  `PUBLIC_BOUNDARY_PASS` for the current tree and selected refs `HEAD` and
  `origin/main`; it does not check other refs or prove compilation, native
  display, or release readiness.
- API/schema risk: public C++ diff and merge-summary structs gain trailing
  fields and require downstream rebuilds. No project schema or graph encoding
  changed.
- Dataset/eval risk: no dataset, labels, prompts, or evaluation expectations
  changed; no cross-product evaluation was run.
- Deployment risk: build and native interaction behavior remain unverified.
- Rollback path: remove the connection graph summaries and diff collection,
  UI rows and summary counts, focused regression assertions, and this ledger
  continuation. Preserve canonical full-snapshot comparison and human merge
  acceptance.
- Intentionally rejected: unbounded graph or evidence retention, treating
  authored compatibility as runtime truth, and adding per-port/edge merge
  authority.
- Cross-product trace: Cartographer only. RegOS, IDA_Training,
  hidden_canopy_bot, and Ask_IDA contracts remain untouched. Dataset/training
  progress is unchanged; capability progress is code-only, and promotion
  remains open.

## Project-authoring standards detail continuation (2026-10-26, code-only)

Merge review now compares project-authoring standards by canonical snapshot
digest and exact record values. It retains at most 64 changed records, reports
exact record counts and truncation, and exposes each standard's label,
category, value, unit, record digest, and optional measurement evidence with a
bounded sample. Rationale text is not copied into the review; the row shows
its byte count and digest instead.

The data remains diagnostic. Full-project digest binding and human
whole-snapshot acceptance remain unchanged; standards do not become
individually mergeable.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/include/carto/ui/ui.hpp`,
  `libs/ui/src/ui.cpp`, `apps/desktop/main.cpp`,
  `tests/agent_workbench_tests.cpp`, this file, and the local work queue.
- Capability gained: reviewers can attribute current/speculative standards
  changes and inspect approved values and measurement evidence before selecting
  a complete snapshot.
- Boundary improved: additions, removals, value/evidence edits, and over-cap
  record samples are identified; rationale bytes remain outside the diff and
  UI detail projection.
- Governance preserved: the comparison does not infer or approve standards;
  existing human standards actions and whole-snapshot acceptance remain the
  mutation paths.
- Tests added: current-side standards addition through the existing human UI
  action, record/category summary, current/speculative attribution, candidate
  count, rationale exclusion, and a 65-record diff capped at 64 summaries.
- Tests run: none, at the user's request for coding without testing.
- Tests not run: compilation, Agent Workbench/UI suites, desktop rendering,
  cross-product suites, and native interaction acceptance.
- Public/private boundary: the read-only scan returned
  `PUBLIC_BOUNDARY_PASS` for the current tree and selected refs `HEAD` and
  `origin/main`; it does not cover other refs or establish compilation or
  release readiness.
- API/schema risk: public C++ diff and merge-summary structs gain trailing
  fields and require downstream rebuilds. No project serialization schema
  changed.
- Dataset/eval risk: no dataset, labels, prompts, or evaluation expectations
  changed.
- Deployment risk: UI rendering and builds remain unverified.
- Rollback path: remove standards diff summaries and collection, UI/candidate
  counts and detail rows, focused assertions, and this ledger continuation.
  Preserve existing human mutation actions and full-snapshot merge binding.
- Intentionally rejected: copying rationale text into the review, inferring
  standards from measurement samples, or introducing per-standard merge
  choices.
- Cross-product trace: Cartographer only. RegOS, IDA_Training,
  hidden_canopy_bot, and Ask_IDA contracts remain unchanged. Dataset/training
  progress is unchanged; capability evidence is code-only, and promotion
  remains open.

## World-model category detail continuation (2026-10-26, code-only)

Merge diffs now identify canonical world-model changes and report before/after
counts for semantic areas, paths, regions, structure graphs, instance fields,
and variant layers, along with digest prefixes. The UI explicitly says that
record-level world-model values are not projected. Equal collection counts do
not imply equal records; the canonical world-model digest and complete project
digest remain available to distinguish changed snapshots.

This is category attribution only. It does not decode world records, infer
geography or runtime behavior, or add world-model mutation/merge authority.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/include/carto/ui/ui.hpp`,
  `libs/ui/src/ui.cpp`, `apps/desktop/main.cpp`,
  `tests/agent_workbench_tests.cpp`, this file, and the local work queue.
- Capability gained: reviewers can distinguish a changed provider-neutral
  world model from other opaque project changes and compare its top-level
  authored collection counts.
- Boundary improved: world-model-only changes and mixed changes remain
  explicitly attributed without pretending that record-level values or
  consumer behavior were reviewed.
- Governance preserved: no world state is inferred, composed, or mutated by
  merge review; complete snapshot selection remains human-gated.
- Tests added: journaled world-path addition and before/after world collection
  counts/digests. The UI can only receive such source changes through existing
  project-level pathways; no new mutation action was introduced.
- Tests run: none, at the user's request for coding without testing.
- Tests not run: compilation, Agent Workbench/UI suites, desktop rendering,
  cross-product suites, and native interaction acceptance.
- Public/private boundary: the read-only audit returned `PUBLIC_BOUNDARY_PASS`
  for the current tree and selected refs `HEAD` and `origin/main`; it does not
  cover other refs or prove compilation or release readiness.
- API/schema risk: public C++ diff and merge-summary structs gain trailing
  fields and require downstream rebuilds. World/project serialization is
  unchanged.
- Dataset/eval risk: no dataset, labels, prompts, or evaluation expectations
  changed.
- Deployment risk: UI and build behavior remain unverified.
- Rollback path: remove the world-model summary, diff detection, UI/candidate
  counts, regression case, and this continuation. Preserve complete snapshot
  digest and human acceptance behavior.
- Intentionally rejected: labeling a world record as changed based only on
  stable collection counts, fabricating geo/runtime interpretation, or adding
  field-level world merge choices.
- Cross-product trace: Cartographer only. RegOS, IDA_Training,
  hidden_canopy_bot, and Ask_IDA contracts and datasets remain unchanged.
  Dataset/training progress is unchanged; capability evidence is code-only,
  and promotion remains open.

## Scientific-model category detail continuation (2026-10-26, code-only)

The merge diff now hashes the canonical serialized scientific model and shows
before/after byte counts, top-level record counts, and populated extension
names. Scientific record values and extension payloads remain unprojected; the
UI labels that limit. This distinguishes a scientific-model change from other
opaque project changes without exposing domain payloads or implying scientific
validity, provider execution, or evaluation acceptance.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/include/carto/ui/ui.hpp`,
  `libs/ui/src/ui.cpp`, `apps/desktop/main.cpp`,
  `tests/agent_workbench_tests.cpp`, this file, and the local work queue.
- Capability gained: reviewers can identify scientific-model changes and
  compare bounded top-level structure on current/speculative/candidate sides.
- Boundary improved: serialization bytes and collection counts are explicit;
  scientific record values and provider/evaluation claims are not inferred.
- Governance preserved: merge review is read-only and adds no provider call,
  scientific evaluation, or per-record merge authority.
- Tests added: a journaled scientific-region addition and before/after model
  counts/serialized-byte summaries.
- Tests run: none, at the user's request for coding without testing.
- Tests not run: compilation, Agent Workbench/UI suites, desktop rendering,
  cross-product suites, and native interaction acceptance.
- Public/private boundary: the selected-ref audit will be rerun after this
  code-only extension; prior passing scope does not establish release readiness.
- API/schema risk: public C++ diff and merge-summary structs gain trailing
  fields and require downstream rebuilds. Scientific/project serialization is
  unchanged.
- Dataset/eval risk: no dataset, labels, prompts, model weights, or evaluation
  artifacts changed.
- Deployment risk: UI and build behavior remain unverified.
- Rollback path: remove the scientific summary and diff detection, UI/candidate
  counts and category row, regression case, and this continuation. Preserve
  full-project digest binding and human whole-snapshot acceptance.
- Intentionally rejected: exposing scientific payloads, claiming provider or
  evaluation execution, or adding independent scientific-record merge choices.
- Cross-product trace: Cartographer only. RegOS, IDA_Training,
  hidden_canopy_bot, and Ask_IDA contracts are unchanged; dataset/training
  progress is unchanged, capability evidence is code-only, and promotion
  remains open.

## Procedural-source detail continuation (2026-10-26, code-only)

The project diff now compares curve paths, road design sources, sweep recipes,
and sign-text sources by complete serialized source records. It retains at most
64 changed records across these families and reports exact changed counts and
per-family before/after record totals. Rows show source revision and digest;
curve segment counts; road centerline, dimensions, grade, radius, and intent;
sweep centerline, profile count, sampling, scale, cap, and inner-profile
settings; and sign-text byte count/digest, alignment, and layout dimensions.
Current/speculative review rows include before/after collection totals, and the
selected-candidate panel repeats those totals. Sign content itself is not
copied into review rows. The project-wide digest continues to bind all source
bytes, and generated mesh output is not treated as the authored source.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/include/carto/ui/ui.hpp`,
  `libs/ui/src/ui.cpp`, `apps/desktop/main.cpp`,
  `tests/agent_workbench_tests.cpp`, this file, and the local queue.
- Capability gained: current, speculative, and candidate summaries can
  attribute four procedural source families and expose bounded, useful source
  parameters without reading generated output as intent.
- Boundary improved: records added, removed, updated, or capped at 64 retain
  exact counts; sign text is represented only by byte count and digest.
- Governance preserved: source comparison is read-only; serialized source
  identity and full-project digests remain authoritative; merge selection
  still selects one exact whole snapshot through the existing human boundary.
- Tests added: journaled additions for all four source families, linked
  centerline and numeric summary checks, sign-text redaction, a 65-path
  truncation case, current/speculative attribution, and candidate collection
  totals. They were authored but not run.
- Tests run: none, at the user's request for a code-only sprint.
- Tests not run: compilation, Agent Workbench/UI suites, desktop rendering,
  cross-product suites, and native interaction acceptance.
- Public/private boundary: refreshed read-only audit result is recorded in the
  local queue: `PUBLIC_BOUNDARY_PASS` for current working tree refs `HEAD` and
  `origin/main`. Other refs and the optional ignore-policy probe are outside
  that result; it does not establish build or release readiness.
- API/schema risk: public C++ diff and UI summary structs gain trailing fields
  and require downstream rebuilds. Project and source serialization are
  unchanged.
- Dataset/eval risk: no dataset, prompts, labels, or evaluation artifacts
  changed.
- Deployment risk: code and UI rendering remain unbuilt and unverified.
- Rollback path: remove the procedural-source summaries, comparison, UI rows
  and counts, regression cases, and this continuation. Keep full-project
  digest binding and existing whole-snapshot human acceptance.
- Intentionally rejected: showing sign text verbatim, inferring runtime road
  or sweep behavior, treating generated geometry as source truth, or adding
  independent per-record merge choices.
- Cross-product trace: direct impact is Cartographer's Agent Workbench merge
  review. RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA have no direct
  contract changes; indirect API rebuild impact applies only to consumers of
  these Cartographer C++ structs. Dataset/training progress is unchanged,
  capability progress is code-only, and promotion remains open.

## Biological-authoring detail continuation (2026-10-26, code-only)

Project diffs now compare opaque biological authoring records by their exact
stored bytes and digest, with a shared 64-record sample cap and exact before,
after, and changed-record counts. The current/speculative UI and selected
candidate summary now include this category, and the selected-candidate review
pane lists its retained bounded records. Per-record rows expose only the source
namespace, asset identity, serialized byte count, and digest prefix; authoring
payload bytes and biological interpretation are not displayed.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/include/carto/ui/ui.hpp`,
  `libs/ui/src/ui.cpp`, `apps/desktop/main.cpp`,
  `tests/agent_workbench_tests.cpp`, this file, and the local queue.
- Capability gained: merge review can attribute opaque biological authoring
  changes to current, speculative, and selected candidate snapshots.
- Boundary improved: additions beyond the 64-record sample remain counted;
  review rows do not copy biological payload bytes or infer semantics.
- Governance preserved: comparison is read-only; project digests remain the
  whole-snapshot authority; the existing human whole-snapshot acceptance path
  remains unchanged. No provider result or biological truth is asserted.
- Tests added: a journaled 65-source cap/detection case and current,
  speculative, and candidate UI attribution plus payload-redaction assertions.
  They were authored but not run.
- Tests run: none, per the user's code-only sprint instruction.
- Tests not run: compilation, all unit suites, lint, frontend/backend suites,
  and native interaction.
- API/schema risk: public C++ diff and UI summary structs gain trailing fields;
  downstream consumers may need rebuilds. Project serialization is unchanged.
- Dataset/eval risk: no datasets, prompts, labels, model weights, or evaluation
  artifacts changed.
- Deployment risk: code and native UI rendering remain unbuilt and unverified.
- Rollback path: remove the biological diff summaries, UI rows and counts,
  tests, and this continuation; retain whole-project digest binding and the
  existing human acceptance boundary.
- Intentionally rejected: exposing authoring payload bytes, deriving anatomy
  or runtime meaning from opaque records, adding provider behavior, or
  allowing independent biological-record merge choices.
- Cross-product trace: direct impact is Cartographer's merge review. RegOS,
  IDA_Training, hidden_canopy_bot, and Ask_IDA contracts are unchanged.
  Dataset/training progress is unchanged, capability evidence is code-only,
  and promotion remains open.

## Opaque sidecar three-way status (2026-10-26, code-only)

Engineering and CONDITION sidecar rows now classify base-relative changes as
current-only, speculative-only, identical edits, or contested. Classification
compares the exact resulting presence, byte count, and digest. It is review
diagnostic only; resolution still selects an exact whole snapshot.

- Files changed: `libs/ui/include/carto/ui/ui.hpp`, `libs/ui/src/ui.cpp`,
  `apps/desktop/main.cpp`, `tests/agent_workbench_tests.cpp`, this file, and
  the local queue.
- Capability gained: reviewers can see when opaque Engineering or CONDITION
  sidecars differ on both branches or agree exactly, and can see sidecar change
  counts for current, speculative, and selected-candidate snapshots.
- Boundary improved: same-result edits are distinguished from differing
  sidecar outputs without decoding opaque payloads.
- Governance preserved: no field-level candidate is composed, no sidecar bytes
  are exposed, and human whole-snapshot selection remains unchanged.
- Tests added: a current-only CONDITION sidecar attribution assertion. It was
  authored but not run; identical and contested cases remain unverified.
- Tests run: none, per the code-only instruction.
- Risks: UI classification and its comparison against retained digests are
  unbuilt and unverified.
- Rollback: remove `sidecar_merge_status`, its row suffixes, regression, and
  this subsection.
- Rejected: parsing or composing CONDITION/Engineering payloads, and silently
  resolving contested sidecars.
- Cross-product trace: Cartographer merge review only. RegOS,
  IDA_Training, hidden_canopy_bot, and Ask_IDA contracts are unchanged.
  Dataset/training progress is unchanged; capability is code-only; promotion
  remains open.

## Rig-module source detail continuation (2026-10-26, code-only)

The comparison now attributes rig-module template additions and revisions by
stable project identity. Each changed row reports the source revision, exact
serialized byte count, and digest; up to 64 changed templates are retained,
with exact counts and truncation state. The current/speculative table and the
selected-candidate summary carry the category. Opaque template bytes and
character interpretation are not exposed, and resolution still selects one
complete snapshot.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/include/carto/ui/ui.hpp`,
  `libs/ui/src/ui.cpp`, `apps/desktop/main.cpp`,
  `tests/agent_workbench_tests.cpp`, this file, and the local work queue.
- Capability gained: merge reviewers can attribute rig-module template
  additions and revisions on current, speculative, and candidate snapshots.
- Boundary improved: stable identity, revision, byte count, digest, and sample
  truncation are explicit while template payload and semantics remain opaque.
- Governance preserved: comparison is read-only; full-project digests remain
  authoritative; no character generation or independent merge choice was added.
- Tests added: insertion and replacement attribution, exact digest/revision
  summaries, and a 65-record cap/truncation case. They were authored but not
  run under the code-only sprint instruction.
- Tests run: none.
- Tests not run: compilation, Agent Workbench/UI suites, lint, desktop
  rendering, and native interaction.
- API/schema risk: public C++ diff and UI summary structs gain additive fields
  and require downstream rebuilds. Project serialization is unchanged.
- Dataset/eval risk: no dataset, prompt, label, model weight, or evaluation
  artifact changed.
- Deployment risk: code and UI rendering remain unbuilt and unverified.
- Rollback: remove the rig-module diff summaries, UI rows/counts, regression,
  and this continuation; preserve full-project digest binding and whole-snapshot
  human acceptance.
- Rejected: exposing serialized template bytes, inferring rig behavior from
  opaque source, or adding field-level resolution authority.
- Cross-product trace: direct impact is Cartographer's Agent Workbench. RegOS,
  IDA_Training, hidden_canopy_bot, and Ask_IDA contracts remain unchanged;
  dataset/training progress is unchanged, capability evidence is code-only,
  and promotion remains open.

## Decal-placement source detail continuation (2026-10-26, code-only)

Source-aware project diffs now attribute decal-placement additions, removals,
and revisions by stable placement ID. Each row reports the exact serialized
byte count and digest for the retained before/after record. The diff preserves
the total changed count and samples at most 64 records; the Agent Workbench
shows the category count, before/after collection sizes, and truncation state
for current, speculative, and selected-candidate snapshots. The placement
payload remains opaque, and resolution continues to select one exact complete
snapshot.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/include/carto/ui/ui.hpp`,
  `libs/ui/src/ui.cpp`, `apps/desktop/main.cpp`, this file, and the local queue.
- Capability gained: reviewers can identify which revision-bound decal
  placements changed on either merge side and in the selected candidate.
- Boundary improved: IDs, revisions, byte counts, digests, exact changed
  counts, and the 64-record cap are visible without decoding placement bytes.
- Governance preserved: comparison remains read-only, complete-project
  digests remain authoritative, and human acceptance still selects an exact
  complete snapshot through the existing journaled path.
- Tests added: none, per the request to code without testing.
- Tests run: none.
- Tests not run: compilation, Agent Workbench/UI suites, lint, desktop rendering,
  and native interaction, per the no-test instruction.
- API/schema risk: public C++ diff and UI summary structs gain additive fields;
  downstream consumers may need rebuilds. Project serialization is unchanged.
- Dataset/eval risk: no datasets, prompts, labels, model weights, or evaluation
  artifacts changed.
- Deployment risk: source changes and desktop projection remain unbuilt and
  unverified.
- Rollback path: remove the decal-placement diff summaries and UI rows/counts,
  then remove this subsection; retain full-project digest binding and
  whole-snapshot acceptance.
- Intentionally rejected: decoding or displaying opaque placement payloads,
  adding independent placement choices, or composing a merged project.
- Cross-product trace: direct impact is Cartographer's Agent Workbench.
  RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA contracts are unchanged;
  dataset/training progress is unchanged, capability evidence is code-only,
  and promotion remains open.

## World-area and path source detail continuation (2026-10-26, code-only)

World-model review now compares areas and paths by stable ID and complete
authored fields. Changed-area rows include semantic type, geometry binding,
parent, bounded tag sample, serialized byte count, and record digest.
Changed-path rows include semantic type, terminal area IDs, width, closed state,
bounded control-point sample, serialized byte count, and record digest. Per-kind
change totals remain exact while each retained record sample is capped at 64;
tags and control points are capped at eight per record. Path equality
distinguishes signed zero to match canonical serialization. Current/speculative
and selected-candidate surfaces show before/after summaries and disclose
truncation or summary unavailability. Instance-field and variant-layer values
were digest/count only at this stage; the follow-on section below adds their
bounded details.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/src/ui.cpp`,
  `apps/desktop/main.cpp`, this file, and the local queue.
- Capability gained: reviewers can attribute semantic-area and path edits to
  stable records across both sides and the selected candidate.
- Boundary improved: record samples and nested tag/control-point samples are
  bounded; signed-zero path edits remain visible; no geometry or runtime
  behavior is inferred.
- Governance preserved: comparison is read-only, complete snapshot digests
  remain authoritative, and acceptance still selects one complete snapshot.
- Tests added: none during this code-only continuation.
- Tests run: none.
- Tests not run: compilation, Agent Workbench/UI suites, lint, desktop
  rendering, and native interaction, per the request to code without testing.
- API/schema risk: additive public C++ diff fields require downstream rebuilds;
  project serialization is unchanged.
- Dataset/eval risk: no datasets, prompts, labels, model weights, or evaluation
  artifacts changed.
- Deployment risk: source and UI projection remain unbuilt and unverified.
- Rollback path: remove area/path comparison fields and review rows, then remove
  this subsection; retain canonical whole-project digest binding and
  whole-snapshot acceptance.
- Intentionally rejected: area/path-level merge resolution, inferred geometry
  semantics, and expanding review into instance-field or variant-layer values
  without a separately bounded projection.
- Cross-product trace: direct impact is Cartographer's Agent Workbench.
  RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA contracts are unchanged;
  dataset and training progress are unchanged, capability evidence is code-only,
  and promotion remains open.

## World instance-field and variant-layer detail continuation (2026-10-26, code-only)

World-model review now compares generated instance fields and variant layers by
stable ID and complete authored values. Instance-field rows include exact
bounds, recipe identity/digest, seed and population limit, suppressed-instance
count with an eight-ID sample, geometry evidence, and materialization intent.
Variant-layer rows include base revision and up to eight edits; each edit shows
target, operation, property, a 64-byte canonical-value sample, full value byte
count, and value digest. Record samples are capped at 64 and exact changed
totals are retained. Current/speculative and selected-candidate views expose
the bounded before/after summaries.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/src/ui.cpp`,
  `apps/desktop/main.cpp`, this file, and the local queue.
- Capability gained: reviewers can trace authored population and variant-layer
  changes across both sides and the selected candidate.
- Boundary improved: nested IDs and edit/value previews are capped and disclose
  truncation; full digests and byte counts preserve source attribution.
- Governance preserved: diffing remains read-only; summaries do not generate
  instances, apply variant edits, or create independent merge choices.
- Tests added: none during this code-only continuation.
- Tests run: none.
- Tests not run: compilation, Agent Workbench/UI suites, lint, desktop
  rendering, and native interaction, per the request to code without testing.
- API/schema risk: additive public C++ diff fields require downstream rebuilds;
  project serialization is unchanged.
- Dataset/eval risk: no datasets, prompts, labels, model weights, or evaluation
  artifacts changed.
- Deployment risk: source and UI projection remain unbuilt and unverified.
- Rollback path: remove instance-field/variant-layer summaries and review rows,
  then remove this subsection; retain whole-project digest binding and
  whole-snapshot acceptance.
- Intentionally rejected: materializing generated instances, applying variant
  operations during review, and granting record-level merge authority.
- Cross-product trace: direct impact is Cartographer's Agent Workbench.
  RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA contracts are unchanged;
  dataset/training progress is unchanged, capability evidence is code-only,
  and promotion remains open.

## World-region source detail continuation (2026-10-26, code-only)

World-model diffs now attribute region additions, removals, and field changes
by stable region ID. Each bounded before/after summary shows revision, grid
cell, exact membership totals, and up to eight IDs for areas, paths, neighbors,
and cross-region paths. The current/speculative projection and selected
candidate display retained rows; at this continuation, other world record
categories remained digest/count only.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/src/ui.cpp`,
  `apps/desktop/main.cpp`, this file, and the local queue.
- Capability gained: reviewers can locate region-level world-model edits and
  inspect bounded membership transitions on each merge side and candidate.
- Boundary improved: full record equality detects same-ID edits; sample caps
  preserve exact totals and disclose omissions.
- Governance preserved: projection is read-only, region values are not
  interpreted as geographic truth, and resolution remains whole-snapshot.
- Tests added: none, per the request to code without testing.
- Tests run: none.
- Tests not run: compilation, Agent Workbench/UI suites, lint, desktop
  rendering, and native interaction, per the no-test instruction.
- API/schema risk: public C++ diff summaries gain additive fields; project
  serialization is unchanged.
- Dataset/eval risk: no datasets, prompts, labels, model weights, or evaluation
  artifacts changed.
- Deployment risk: source and UI projection remain unbuilt and unverified.
- Rollback path: remove world-region summary collection and UI rows, then
  remove this subsection; retain whole-project digest binding.
- Intentionally rejected: geographic interpretation, region-level resolution,
  and composition of a merged world model.
- Cross-product trace: direct impact is Cartographer's Agent Workbench.
  RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA contracts are unchanged;
  dataset/training progress is unchanged, capability evidence is code-only,
  and promotion remains open.

## Semantic sidecar attribution continuation (2026-10-26, code-only)

Opaque semantic sidecar changes now carry before/after presence, byte count,
and stored digest in current/speculative detail rows and the selected-candidate
summary. Both serialized bytes and the separately stored digest participate in
change detection. Sidecar content remains opaque and is bound by the complete
project digest.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/include/carto/ui/ui.hpp`,
  `libs/ui/src/ui.cpp`, `apps/desktop/main.cpp`, this file, and the local queue.
- Capability gained: reviewers can attribute semantic sidecar presence and
  binding changes rather than seeing only a whole-snapshot change.
- Boundary improved: byte and stored-digest changes are detected independently;
  the projection exposes metadata without parsing the sidecar.
- Governance preserved: diffing remains read-only, opaque content is not
  interpreted, and merge acceptance remains a whole-snapshot operation.
- Tests added: none, per the request to code without testing.
- Tests run: none.
- Tests not run: compilation, Agent Workbench/UI suites, lint, desktop
  rendering, and native interaction, per the no-test instruction.
- API/schema risk: additive public C++ diff and UI summary fields may require
  downstream rebuilds; project serialization is unchanged.
- Dataset/eval risk: no datasets, prompts, labels, model weights, or evaluation
  artifacts changed.
- Deployment risk: source and UI projection remain unbuilt and unverified.
- Rollback path: remove semantic-sidecar comparison and UI summary fields, then
  remove this subsection; preserve complete-project digest binding.
- Intentionally rejected: parsing sidecar semantics, deriving runtime state,
  or enabling sidecar-level merge resolution.
- Cross-product trace: direct impact is Cartographer's Agent Workbench.
  RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA contracts are unchanged;
  dataset/training progress is unchanged, capability evidence is code-only,
  and promotion remains open.

## Opaque mesh-authoring source diff continuation (2026-10-26, code-only)

SculptStack, GarmentAsset, and cloth rest-pose records are now compared by mesh
asset ID. The current/speculative projection and selected-candidate summary
show per-kind totals and bounded before/after byte-count/digest rows. Serialized
source bytes remain opaque, and whole-project digests still bind the complete
snapshot.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/include/carto/ui/ui.hpp`,
  `libs/ui/src/ui.cpp`, `apps/desktop/main.cpp`, this file, and the local queue.
- Capability gained: reviewers can attribute changes to project-bound sculpt,
  garment, and cloth records without relying on a digest-only warning.
- Boundary improved: records use stable mesh IDs, exact equality, exact
  per-kind totals, and a shared 64-row cap.
- Governance preserved: source bytes are not decoded, diffing is read-only,
  and acceptance remains a whole-snapshot journaled operation.
- Tests added: none, per the request to code without testing.
- Tests run: none.
- Tests not run: compilation, Agent Workbench/UI suites, lint, desktop
  rendering, and native interaction, per the no-test instruction.
- API/schema risk: additive public C++ diff and UI summary fields may require
  downstream rebuilds; project serialization is unchanged.
- Dataset/eval risk: no datasets, prompts, labels, model weights, or evaluation
  artifacts changed.
- Deployment risk: source and UI projection remain unbuilt and unverified.
- Rollback path: remove the mesh-authoring record collection and UI summaries,
  then remove this subsection; preserve full-project digest binding.
- Intentionally rejected: parsing or composing sculpt/garment/cloth records,
  or granting per-record merge authority.
- Cross-product trace: direct impact is Cartographer's Agent Workbench.
  RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA contracts are unchanged;
  dataset/training progress is unchanged, capability evidence is code-only,
  and promotion remains open.

## Topology and replay-patch receipt continuation (2026-10-26, code-only)

Historical topology receipts and persistent mesh-patch records now join weight
and keyed-curve receipts in the bounded sequence diff. Topology rows bind the
mesh asset and canonical receipt serialization digest. Patch rows retain patch,
source-mesh, and result-mesh digests plus serialized byte counts. Sequence
position remains attribution only, and payloads are not decoded or composed.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/src/ui.cpp`,
  `apps/desktop/main.cpp`, this file, and the local queue.
- Capability gained: merge review can locate changes to durable topology
  history and revision-bound replay patches alongside other edit receipts.
- Boundary improved: each history kind remains distinct, exact receipt
  comparisons feed totals, and retained rows stay within the shared 64-record
  cap.
- Governance preserved: comparison remains read-only, patch bytes are opaque,
  full-project digests remain authoritative, and acceptance chooses one whole
  snapshot.
- Tests added: none, per the request to code without testing.
- Tests run: none.
- Tests not run: compilation, Agent Workbench/UI suites, lint, desktop
  rendering, and native interaction, per the no-test instruction.
- API/schema risk: public C++ receipt summaries gain additive fields; project
  serialization is unchanged.
- Dataset/eval risk: no datasets, prompts, labels, model weights, or evaluation
  artifacts changed.
- Deployment risk: source and UI projection remain unbuilt and unverified.
- Rollback path: remove topology/patch sequence collection and its additional
  UI fields, then remove this subsection; retain whole-project digest binding
  and whole-snapshot acceptance.
- Intentionally rejected: decoding patch payloads, replaying patches during
  review, or allowing history-level resolution.
- Cross-product trace: direct impact is Cartographer's Agent Workbench.
  RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA contracts are unchanged;
  dataset/training progress is unchanged, capability evidence is code-only,
  and promotion remains open.

## Generated-instance provenance diff continuation (2026-10-26, code-only)

Source-aware diffs now compare sign-text, road-surface, biological-proxy, and
CONDITION vegetation instance receipts by object ID. Comparison includes every
persisted receipt field, including both road and centerline source lineage,
vegetation field receipts, and generated-product digests. The UI exposes exact
per-kind counts and up to 64 before/after summaries. Snapshot bytes remain
opaque; summaries retain source identities, revisions, byte counts, digests,
field semantics, and materialized placement values as applicable. Full-project
digests remain authoritative, and resolution remains whole-snapshot.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/include/carto/ui/ui.hpp`,
  `libs/ui/src/ui.cpp`, `apps/desktop/main.cpp`, this file, and the local queue.
- Capability gained: reviewers can trace generated sign, road, biological, and
  vegetation instances back to their retained source receipts on each side.
- Boundary improved: receipt equality compares all persisted fields; output is
  bounded and avoids exposing embedded source snapshots.
- Governance preserved: diffing is read-only, receipt history is not rewritten,
  no biological provider/runtime result is inferred, and human resolution
  selects one complete snapshot.
- Tests added: none, per the request to code without testing.
- Tests run: none.
- Tests not run: compilation, Agent Workbench/UI suites, lint, desktop rendering,
  and native interaction, per the no-test instruction.
- API/schema risk: public C++ diff and UI summary structs gain additive fields;
  downstream consumers may need rebuilds. Project serialization is unchanged.
- Dataset/eval risk: no datasets, prompts, labels, model weights, or evaluation
  artifacts changed.
- Deployment risk: source and UI projection remain unbuilt and unverified.
- Rollback path: remove the instance-receipt diff summaries and UI rows/counts,
  then remove this subsection; preserve whole-project digest binding and
  whole-snapshot acceptance.
- Intentionally rejected: displaying embedded snapshots, interpreting receipt
  payloads as provider output, or allowing independent receipt merges.
- Cross-product trace: direct impact is Cartographer's Agent Workbench.
  RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA contracts are unchanged;
  dataset/training progress is unchanged, capability evidence is code-only,
  and promotion remains open.

## Project metadata diff continuation (2026-10-26, code-only)

The diff now classifies project-name, source-namespace, and evaluation-graph
digest changes. The current/speculative rows display exact string transitions
and evaluation-graph digest prefixes; the selected-candidate summary counts
changed fields. This adds attribution for project metadata changes that were
previously visible only through the complete-project digest. Resolution still
selects an exact whole snapshot.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/include/carto/ui/ui.hpp`,
  `libs/ui/src/ui.cpp`, `apps/desktop/main.cpp`, this file, and the local queue.
- Capability gained: reviewers can distinguish project-label, source-scope,
  and evaluation-graph binding changes in each branch and candidate.
- Boundary improved: metadata changes are classified without interpreting or
  regenerating the evaluation graph.
- Governance preserved: the diff is read-only, full-project digests remain
  authoritative, and human resolution continues to select a complete snapshot.
- Tests added: none, per the request to code without testing.
- Tests run: none.
- Tests not run: compilation, Agent Workbench/UI suites, lint, desktop rendering,
  and native interaction, per the no-test instruction.
- API/schema risk: public C++ diff and UI summary structs gain additive fields;
  downstream consumers may need rebuilds. Project serialization is unchanged.
- Dataset/eval risk: no datasets, prompts, labels, model weights, or evaluation
  artifacts changed; the evaluation-graph digest is only displayed as a binding.
- Deployment risk: source and UI projection remain unbuilt and unverified.
- Rollback path: remove project metadata fields and UI rows/counts, then remove
  this subsection; retain full-project digest binding and whole-snapshot
  acceptance.
- Intentionally rejected: interpreting graph contents, evaluating a changed
  graph during diff, or adding metadata-level merge choices.
- Cross-product trace: direct impact is Cartographer's Agent Workbench.
  RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA contracts are unchanged;
  dataset/training progress is unchanged, capability evidence is code-only,
  and promotion remains open.

## Historical edit-receipt detail continuation (2026-10-26, code-only)

Project diffs now compare the append-only weight-edit and keyed-curve receipt
sequences by kind and sequence position. Exact per-kind before/after counts and
total changed count are retained, with a shared 64-record detail cap. Each row
reports before/after serialized byte count and digest; payload bytes remain
opaque. Sequence position is review attribution, while complete-project
digests continue to bind the authoritative snapshots. Resolution remains a
whole-snapshot choice.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/include/carto/ui/ui.hpp`,
  `libs/ui/src/ui.cpp`, `apps/desktop/main.cpp`, this file, and the local queue.
- Capability gained: merge review can locate changed historical weight and
  keyed-curve receipts and compare each side's counts and digest summaries.
- Boundary improved: receipt kinds remain separate and only bounded metadata
  is projected; the diff does not interpret or rewrite history.
- Governance preserved: comparison is read-only, sequence position is not
  promoted to a durable identity, and human resolution selects an exact
  complete snapshot.
- Tests added: none, per the request to code without testing.
- Tests run: none.
- Tests not run: compilation, Agent Workbench/UI suites, lint, desktop rendering,
  and native interaction, per the no-test instruction.
- API/schema risk: public C++ diff and UI summary structs gain additive fields;
  downstream consumers may need rebuilds. Project serialization is unchanged.
- Dataset/eval risk: no datasets, prompts, labels, model weights, or evaluation
  artifacts changed.
- Deployment risk: source and UI projection remain unbuilt and unverified.
- Rollback path: remove the historical receipt diff summaries and UI rows/counts,
  then remove this subsection; preserve full-project digest binding and
  whole-snapshot acceptance.
- Intentionally rejected: decoding receipt payloads, merging history sequences,
  or allowing independent receipt choices.
- Cross-product trace: direct impact is Cartographer's Agent Workbench.
  RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA contracts are unchanged;
  dataset/training progress is unchanged, capability evidence is code-only,
  and promotion remains open.

## Pose-library source detail continuation (2026-10-26, code-only)

Source-aware project diffs now attribute pose-library additions, removals, and
revisions by stable library identity. Each retained row reports the exact
serialized byte count and digest for before and after. The diff preserves exact
changed totals and samples at most 64 libraries; current/speculative and
selected-candidate projections show the category and truncation. Pose-library
bytes remain opaque, and merge resolution still chooses one exact complete
snapshot.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/include/carto/ui/ui.hpp`,
  `libs/ui/src/ui.cpp`, `apps/desktop/main.cpp`, this file, and the local queue.
- Capability gained: reviewers can attribute changed pose-library identities
  across current, speculative, and selected-candidate snapshots.
- Boundary improved: stable identity, source revision, byte count, digest, and
  bounded-sample truncation are explicit without decoding pose bytes.
- Governance preserved: the projection is read-only, digests bind complete
  project snapshots, and the human whole-snapshot journal path is unchanged.
- Tests added: none, per the request to code without testing.
- Tests run: none.
- Tests not run: compilation, Agent Workbench/UI suites, lint, desktop rendering,
  and native interaction, per the no-test instruction.
- API/schema risk: public C++ diff and UI summary structs gain additive fields;
  downstream consumers may need rebuilds. Project serialization is unchanged.
- Dataset/eval risk: no datasets, prompts, labels, model weights, or evaluation
  artifacts changed.
- Deployment risk: source and UI projection remain unbuilt and unverified.
- Rollback path: remove the pose-library diff summaries and UI rows/counts,
  then remove this subsection; preserve complete-project digest binding and
  whole-snapshot acceptance.
- Intentionally rejected: parsing pose content, deriving rig behavior,
  introducing field-level resolution, or composing a candidate.
- Cross-product trace: direct impact is Cartographer's Agent Workbench.
  RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA contracts are unchanged;
  dataset/training progress is unchanged, capability evidence is code-only,
  and promotion remains open.

## World structure-graph source detail continuation (2026-10-26, code-only)

World-model diffs now compare structure graphs by stable graph ID, including
semantic type and every node/edge field. Changed graph summaries bind a
deterministic single-record serialization digest, byte count, exact node/edge
totals, and up to eight node and edge IDs. Current/speculative and selected
candidate views show the bounded before/after metadata; graph bytes and
behavior remain uninterpreted.

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `libs/ui/src/ui.cpp`,
  `apps/desktop/main.cpp`, this file, and the local queue.
- Capability gained: reviewers can identify same-ID graph edits and trace
  changed node/edge sets across each merge side and selected candidate.
- Boundary improved: exact comparisons cover graph identity, node identity and
  area bindings, and edge endpoints, relationship, path binding, and direction.
  Samples are capped and exact changed totals are retained.
- Governance preserved: diffing is read-only; graph behavior is not inferred;
  full-project digests remain authoritative and acceptance remains
  whole-snapshot.
- Tests added: none, per the request to code without testing.
- Tests run: none.
- Tests not run: compilation, Agent Workbench/UI suites, lint, desktop
  rendering, and native interaction, per the no-test instruction.
- API/schema risk: additive public C++ diff fields may require downstream
  rebuilds; project serialization is unchanged.
- Dataset/eval risk: no datasets, prompts, labels, model weights, or evaluation
  artifacts changed.
- Deployment risk: source and UI projection remain unbuilt and unverified.
- Rollback path: remove the graph comparison and UI summaries, then remove this
  subsection; retain whole-project digest binding.
- Intentionally rejected: deriving execution semantics from graph records,
  composing graph changes, or granting graph-level merge choices.
- Cross-product trace: direct impact is Cartographer's Agent Workbench.
  RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA contracts are unchanged;
  dataset/training progress is unchanged, capability evidence is code-only,
  and promotion remains open.
