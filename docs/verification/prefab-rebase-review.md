# CONDITION prefab rebase review

## Implemented slice

`condition::PrefabRebaseReview::between` produces a read-only, authoring-bound
delta for an instance and its current prefab definition. It reports added and
removed members, changed source bindings, and source evidence that is no longer
current. The review records the authoring revision and canonical digest, and
`validate` rejects stale or altered review contents by recomputing the delta.
The batch API validates and digests one authoring snapshot once before building
all instance reviews, avoiding a full-document validation/hash for every row.
CONDITION schema 11 records the accepted source-mesh digest and isolated
instance-mesh revision separately. Schema-10 receipts migrate with their former
source/instance equality; schema-9 records remain unisolated.

The application snapshot now includes the review summary for each prefab
instance. The native CONDITION panel lists member and source-binding changes
while keeping the project-mesh, local-override, and membership-choice review
gate visible. An empty authoring delta is not a currentness or rebase approval.

The native panel now collects explicit geometry and placement choices for
retained members, inclusion choices for added members, detach/delete choices
for removed members, and a human rationale. The application action is bound to
project generation/revision and the reviewed authoring revision/digest. It
revalidates current source evidence and isolated instance ownership, refuses
legacy geometry inference and unsupported sculpt/garment transfer, and stages
scene, mesh, material-assignment, and authoring changes in one candidate
document. One `ReplaceProjectDocumentCommand` commits the result through the
existing human-only journal boundary. Schema 12 stores the accepted digest,
rationale, and complete per-member decision set in a `PrefabRebaseReceipt`.
Added and refreshed snapshots share one bounded instance-mesh budget. Removed
mesh assets are included in the command's affected set.

## Boundaries and remaining work

The review delta remains authoring-only; the applying command separately
compares live meshes and scene transforms before accepting any user choice.
Replacing a retained member's geometry preserves its existing assignments;
the command fails closed if the replacement makes an assignment invalid.
Copying source assignments to a newly added or legacy-cloned snapshot also
respects restricted variant mesh scope. Typed socket resolution remains a
separate read-only primitive. Project-schema-34 connection authoring now uses
that primitive through human graph commands; native/runtime connection
acceptance remains open. See the subsequent executed evidence below.

Legacy/unisolated instances can receive an authoring delta summary, but the
application continues to mark them stale and publication remains blocked. This
review does not infer historical geometry from current source data.

## Acceptance evidence

`tests/prefab_rebase_tests.cpp` covers membership additions/removals, batch
review equivalence, stale source evidence, changed review contents, and stale
authoring receipts. `tests/condition_prefab_snapshot_tests.cpp` now covers
schema 9–11 migrations plus schema-12 receipt round trip and invalid decisions.
`tests/condition_prefab_placement_tests.cpp` covers snapshot wiring, empty-delta
gating, removal, explicit source geometry, rationale refusal, detached world
transform preservation, and undo/redo receipt history. These tests were
authored but not built or run. No native UI interaction was performed.

## Repair ledger

- Files: rebase review model, authoring schema/codec and migrations, application
  currentness/snapshot/action, native prefab panel, focused model/application
  tests, and this verification note.
- Capability: operators can inspect the exact source delta, resolve every
  retained/member-change choice, and apply a rationale-backed rebase as one
  undoable project command.
- Invariants: human admission, project and authoring revision/digest checks,
  source and clone receipt validation, explicit member decisions, durable
  decision lineage, candidate atomicity, material variant scope, and bounded
  mesh work remain enforced.
- Risks: code and tests are unbuilt; the native interaction is unverified;
  external older readers limited to schema 11 cannot read schema-12 payloads;
  unsupported appearance/geometry transfer remains refused.
- Rollback: revert the source changes only after preserving a project backup;
  older code cannot downgrade serialized schema-12 receipts automatically.
  Review rows are derived, while accepted rebase decisions are serialized.
- Rejected: automatic geometry or placement overwrite, silent membership
  changes, inferring legacy geometry from the latest source, widening material
  variant scope, and treating a clean authoring diff as complete rebase approval.

## Subsequent executed integration verification (2026-10-25)

The later coding continuation built the native desktop and passed the rebase,
socket, placement, and public API suites within 12/12 selected tests (17.75 s).
The schema-12 snapshot/migration suite was rebuilt and passed separately
(1/1, 0.21 s). Application/UI binaries passed after their final links
(2/2, 22.55 s). These results supersede the earlier unbuilt status for those
tests; native interaction and runtime attachment remain unverified.

Integration fixes add the affected-mesh receipt field already used by the
rebase command, take project-generation/revision fields from the whole snapshot,
and select the exact authored prefab rather than assuming the first row.
The receipt regression checks removed and retained mesh identities. Separate
connection deletion/reopen and journal-refusal acceptance is recorded in
[connection edge history](connection-edge-history.md). Existing authority,
rationale, per-member decision, source/digest, candidate publication, and journal
paths are preserved. Revert only these integration/test hunks if rolling them
back; do not remove the field while keeping callers that require it.
