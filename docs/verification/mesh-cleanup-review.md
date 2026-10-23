# Bounded mesh cleanup review

The Modify instrument can propose a merge-by-distance cleanup for an explicit
vertex selection on one mesh object. The proposal shows the exact source,
merge tolerance in mesh-local units, removed-to-surviving vertex IDs, modified
and removed face IDs, edge/corner identity changes, and source/result geometry
digests. An operator supplies a reason and
applies or discards the reviewed proposal.

## Source and work bounds

- A proposal requires one visible, unlocked object with a uniquely owned mesh.
- Candidate IDs must be distinct and present. The default and hard maximum is
  4,096 candidates and 4,096 merged vertices; at most 64 removed faces may be
  approved. Callers may lower these budgets.
- Inspection bounds the total source vertices, edges, faces, and corners to
  100,000 before copying or hashing the mesh. Owner inspection also refuses a
  scene above 100,000 objects. Exceeding a bound refuses the entire proposal.
- Attributed meshes require an explicit attribute-transfer implementation and
  are refused. Authoring payloads are retained rather than dropped for repair.
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
recomputes the candidate, and compares both its geometry digest and complete
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
  an ambiguous-origin fixture ensures provenance is not guessed.
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

The broader cleanup backlog still includes additional repair types, attribute
transfer, richer issue discovery, and native interaction acceptance. These
limits remain visible in the local work queue.

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

- Files: mesh kernel and topology tests; application review API/implementation
  and tests; command bus and history tests; UI controller, desktop Modify
  instrument, and UI tests; capability matrix and this verification note.
- Capability: inspect a bounded duplicate-vertex repair and explicitly accept
  the exact reviewed topology through durable, reversible authoring history.
- Invariants: read-only proposals; human acceptance; session, source and
  revision binding; complete stable-ID receipts; fail-closed ownership and
  attribute handling; durable append before history finalization.
- Risks: synchronous proposals can pause the UI; the work caps can refuse
  otherwise valid large meshes. Native responsiveness and interaction are not
  yet accepted. The feature implements selected-vertex welding only.
- Rollback: revert the cleanup review changes and the accompanying kernel
  fixes. There are no dependencies, migrations, or project schema changes.
  Saved merge patches already use the existing public replay format.
- Rejected: automatic repair, AI approval, silent attribute loss, modifying a
  shared mesh through one owner, guessed corner provenance, and readiness
  claims based only on compilation or unit tests.
