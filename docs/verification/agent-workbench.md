# Agent Workbench verification contract

## End-to-end fixture

- Create a project with separate mesh objects.
- Capture exact face targets and queue native inset/extrude steps.
- Run the plan on the speculative session.
- Confirm the source revision and geometry remain unchanged before review.
- Confirm per-step receipts, project/viewport gates, and structured mesh diffs.
- Expand changed meshes and inspect all changed vertex positions and face-connectivity identities, including added/removed faces; large lists must remain scrollable without silently truncating evidence.
- Accept the result and confirm the source advances once and the saved-project journal contains one application mutation event.

## Adversarial boundaries

- A target outside objective scope is rejected before execution.
- A protected target is rejected before execution.
- A target mesh shared by an out-of-scope or protected instance is rejected, and all in-scope instances count toward the touched-object budget.
- A topology edit is blocked when the envelope permits geometry only.
- A plan based on a stale source revision cannot start.
- A missing goal cannot start a task, and a source edit after speculative execution prevents acceptance without overwriting the newer source.
- An invalid native operation blocks the task without changing the source.
- Acceptance before gates pass, repeated execution, or rejection after acceptance is invalid.
- The UI captures only a single exact face or vertex per step and exposes explicit accept/reject actions.
- Distinct geometry results produce distinct digests even when their mesh counts and revision shape match.

## SceneQuery coverage

- Exact object ID, stable name, and mesh-asset queries return copied identities at the source revision.
- Name, spatial, topology, and CONDITION results are limited to the task scope; an explicit out-of-scope identity request fails closed.
- Proximity results report world object-origin distance; orientation results report computed world-space normals; boundary-edge results come from validated mesh topology.
- Missing CONDITION sidecars return an explicit unavailable diagnostic, and CONDITION data with a stale project revision is rejected instead of being shown as current.
- Query count, object scans, topology elements, semantic records, result count, and result-byte limits truncate or reject deterministically without mutating the source.
- Invalid query parameters fail before consuming query budget.

## Capability registry coverage

- Every known native mesh tool has a complete typed contract; kernel/ID drift remains unclassified and cannot be provider-requestable.
- Parameter names match the native tool, numeric bounds are finite and coherent, and length/factor/count units are explicit.
- Geometry-only envelopes expose vertex positioning while excluding topology-changing previews; unsupported registered tools never appear in the planner projection.
- The projected context contains no command callback, project document, or direct mutation handle.

## Objective and clarification coverage

- Constraint and question identities are unique, all referenced objects and constraints are in scope, and malformed bounds or candidate answers are rejected.
- A consequential unresolved question blocks task creation without creating or mutating a speculative branch; only the explicit human application boundary can issue an answer receipt, and a selected candidate cannot contradict queued targets.
- The native UI can queue an operator-required question, refuses to run while it is unanswered, and records the explicit answer in the same objective/revision context shown in review.
- The answer receipt is invalidated by a changed plan intent, target, order, or dependency graph, even when the objective text is unchanged.
- Changing the goal or source revision after an answer invalidates the workflow; a failed unresolved attempt does not consume the next task identity.
- Reordering identity lists does not change the canonical objective digest, while changing an answer or hard constraint does.
- Topology-preservation constraints reject a topology capability before speculative execution, and displacement gates block an over-limit result.
- Acceptance journal evidence records both the objective digest and the result diff digest.

## Plan DAG coverage

- Dependencies may point forward or backward in the declared list; scheduling is stable and topological.
- Missing, duplicate, self, and cyclic dependencies are rejected before the speculative session is created.
- A failed step invalidates its dependent branch, while independent ready steps can complete on the isolated branch.
- Any failed or invalidated step keeps acceptance blocked, even when an independent branch produced a valid diff.
- The UI captures explicit dependencies and displays each branch's final status and failure evidence.

## Bounded repair coverage

- Repair starts only from a blocked task on the same, unchanged source session; a stale source revision cannot be reused.
- Repair requires a non-empty operator justification, a new task identity, and a changed step/dependency graph.
- The objective question context, object scope, hard constraints, protected objects, and permitted effects cannot be expanded or weakened.
- Every numeric work/query/retry limit stays the same or narrows; additional operations and repair attempts are bounded across the chain.
- Topology churn counts added/removed mesh assets, vertices, faces, and face connectivity changes; exceeding its envelope blocks acceptance with a bound diagnostic.
- A repaired task starts from the original source state, not the failed speculative branch, and preserves earlier step receipts, failed gate results, and evidence digests.
- Editing a step invalidates plan-bound human clarification receipts; execution remains blocked until the operator answers again for the revised plan.
- The repaired happy path can pass gates and still requires the existing explicit source-acceptance boundary.

## Human ghost correction coverage

- A correction is admitted only through the human UI boundary for an executed review or blocked branch; the source revision must still match the task base.
- Corrections require an in-scope, unprotected, unlocked root object, a finite changed transform, and a non-empty bounded operator reason. Out-of-scope, non-finite, empty-reason, and stale-source attempts leave the speculative revision unchanged.
- A correction mutates only the speculative session, refreshes the ProjectDiff and rendered after-scene, and re-runs acceptance gates. Acceptance remains blocked when any required gate fails.
- Each correction receipt binds the task identity, object, before/after transforms, source and branch revisions, plan context, parent evidence digest, and operator reason.
- Repair, same-source resume, and disjoint-source rebase replay the latest locked transform only when the replayed branch still has the recorded before transform. Repair lineage binds the inherited correction chain; replay drift blocks acceptance.
- Checkpoint format version 3 persists correction evidence and reads version 1 and 2 files without correction evidence. Loading remains non-mutating until explicit replay and acceptance.

## Persistent task checkpoint coverage

- Checkpoints are immutable, versioned files with a checksum, source and speculative project digests, objective/plan context digest, work envelope, step progress, failure receipts, repair lineage, last diff digest, validation report, and provider identity.
- Checkpoint loading validates size bounds, schema version, project documents, source revision, scope equality, enum values, progress, and digest bindings before exposing workflow state.
- A checkpoint is workflow state. Loading it does not replace or mutate the live project document.
- Human clarification answers and their private receipts are removed from checkpoint plans; consequential questions must be answered again through the human application boundary.
- Same-source resume requires the unchanged source revision and complete serialized source digest, then replays the native plan in a new speculative session. The resulting artifact and diff digests must match before a prior review task can remain acceptable.
- An explicit rebase analysis derives object and mesh read/write sets from objective scope, target objects, mesh bindings, and target ancestry. Disjoint object or mesh edits can be rebased; changed scope/ancestor objects, scoped meshes, target write meshes, or a new out-of-scope instance of a written shared mesh block.
- CONDITION or engineering sidecar changes block rebase while field-level query provenance is unavailable. Rebase requires a plan bound to the exact current revision, unchanged intent/scope/dependencies, and renewed human clarification answers.
- A conflict-free rebase replays the complete native plan against the current source and compares the saved and replayed task write-set objects/meshes. Replay drift blocks acceptance. The accepted result includes all unrelated source edits because the speculative branch starts from the current source.
- Rebase lineage records the previous/current source revisions and digests, canonical read/write set digests, and bounded identities. Checkpoint format version 3 reads version 1 and 2 checkpoints without newer correction evidence.
- Rebase currently replays the full plan conservatively; per-step invalidation and re-execution from a minimal affected DAG frontier are not implemented.
- Accepted and rejected checkpoints remain inspectable history and cannot be resumed. Checkpoint writes do not replace an existing file, preserving prior task history.
- Corrupted files, attempts to overwrite a checkpoint, conflicting source changes, missing reapproval, and replayed write-set drift are blocked without changing the live project.
- A disjoint source edit can be explicitly rebased, retained in the resulting source, and accepted only after the plan is reauthorized for the new revision.

## Cross-workspace task status

- A review-ready task remains available from other authoring workspaces with passed-step and required-gate counts derived from its current snapshot.
- A blocked task remains visible, exposes its diagnostic on the task action tooltip, and routes back to the Agent workspace; the strip itself does not accept, repair, or mutate the task.
- The strip is hidden in the Agent workspace, after acceptance/rejection, and when no task exists.
- Native-shell smoke coverage should click the global “Ask Cartographer” entry from Project, Market, and Model surfaces, then confirm Agent workspace navigation and goal-field focus without changing task or project state.

## Follow-up coverage

Add failure injection for durable journal append rollback, unrelated and conflicting source edits during a future rebase flow, bounded repair exhaustion, semantic-sidecar size pressure, and provider timeouts as those tranches are implemented. Clarification questions and answers are reviewable evidence, not natural-language commands; the workbench still does not compile free-form goals or infer questions automatically.
