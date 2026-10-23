# Project diff and merge review

## Current source review

`agent::ProjectDiff::between` binds both complete `ProjectDocument` snapshots
with their canonical serialization digests and revisions. `has_changes()`
checks those digests in addition to the detailed object, mesh, material,
texture, Engineering, and CONDITION rows. A change to another authored source
domain therefore cannot be reported as an empty project diff.

The Agent Workbench shows both source revisions and digest prefixes. It labels
the detailed rows as a partial projection of each project snapshot and warns
when a source-only change has no detailed row. The full digests remain the
identity of the reviewed snapshots; they are not a substitute for decoding
the changed values.

Checkpoint rebase remains conservative: object, mesh, and material-region
read/write conflicts block safe rebase, and the speculative result stays
separate from the live project until explicit acceptance. Existing rebase
evidence covers source advancement, invalidated steps, and disjoint writes.

## Acceptance evidence

- `project_diff_detects_unclassified_project_source_changes` changes only a
  journaled spatial field and verifies that the complete snapshot digests
  differ, `has_changes()` is true, and the diff reports an unclassified-only
  source change. Comparing a document with itself reports no such change.
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

This is bounded evidence for CONDITION state branches. It does not complete a
project-wide diff/merge: arbitrary authored domains still lack generic
base/left/right value rows and resolution choices, and no native interaction
capture proves the desktop controls visually.

## Still incomplete

The CONDITION state-branch slice above does not make this a general project
merge UI. The diff does not yet decompose every authored domain, and the UI does
not load base/left/right project snapshots for a generic three-way comparison.
There is no resolved merged candidate for arbitrary project data. Paired
whole-project digests bind the sides but do not reveal field values or prove
that conflicts were resolved. Contested authored data must not be auto-merged;
completing the queue item requires source-aware conflict rows, explicit
choices, stale-source rejection, and acceptance through the existing project
transaction boundary.

## Repair ledger

- Files changed: `libs/agent/include/carto/agent/workbench.hpp`,
  `libs/agent/src/workbench.cpp`, `apps/desktop/main.cpp`,
  `tests/agent_workbench_tests.cpp`.
- Capability gained: full-document differences cannot be mistaken for an
  empty diff; reviewed snapshot identities are visible on both sides.
- Invariants preserved: source revision/digest binding, conservative rebase,
  speculative/live separation, and explicit human acceptance.
- Tests run: `cartographer_agent_workbench_tests` build; CTest 1/1 passed.
- Tests not run: desktop native-window rendering and an end-to-end generic
  three-way merge acceptance test; that workflow does not exist yet.
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
