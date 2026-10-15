# Agent Workbench verification contract

## End-to-end fixture

- Create a project with separate mesh objects.
- Capture exact face targets and queue native inset/extrude steps.
- Run the plan on the speculative session.
- Confirm the source revision and geometry remain unchanged before review.
- Confirm per-step receipts, project/viewport gates, and structured mesh diffs.
- Expand changed meshes and inspect all changed vertex positions and face-connectivity identities, including added/removed faces; large lists must remain scrollable without silently truncating evidence.
- Accept the result and confirm the source advances once and the saved-project journal contains one application mutation event.
- From Model, Material, CONDITION, and Review, use the global Ask Cartographer entry and confirm the Agent Workbench instrument opens with the goal field focused while preserving the selected object or component and source revision.
- From a Scene outliner row, use the context menu to ask Cartographer about that object; confirm the row's object becomes the sole selected target and no plan or task starts automatically.
- From a review-ready persistent task strip, choose Review result and confirm the Review workspace opens the Agent Workbench with the speculative after branch visible.

## Adversarial boundaries

- A target outside objective scope is rejected before execution.
- A protected target is rejected before execution.
- A target mesh shared by an out-of-scope or protected instance is rejected, and all in-scope instances count toward the touched-object budget.
- A topology edit is blocked when the envelope permits geometry only.
- A plan based on a stale source revision cannot start.
- A missing goal cannot start a task, and a source edit after speculative execution prevents acceptance without overwriting the newer source.
- An invalid native operation blocks the task without changing the source.
- Acceptance before gates pass, repeated execution, or rejection after acceptance is invalid.
- The UI captures only a single exact object, face, or vertex per step and exposes explicit accept/reject actions.
- Object translation is absent from the generic AI proposal path, requires an explicit Agent Workbench transform permission and positive cumulative meter bound, and cannot change the source before human acceptance.
- Translation rejects multiple selected objects, missing transform permission, an invalid or exceeded distance bound, and an objective `preserve_transform` constraint; a successful root translation changes the object transform while preserving rotation, scale, and mesh content.
- Distinct geometry results produce distinct digests even when their mesh counts and revision shape match.

## SceneQuery coverage

- Exact object ID, stable name, and mesh-asset queries return copied identities at the source revision.
- Name, spatial, topology, and CONDITION results are limited to the task scope; an explicit out-of-scope identity request fails closed.
- CONDITION tag and familiarity facts include their source ID, revision, current/reviewed flags, and protected features where applicable; stale or unreviewed source evidence is never projected as current.
- CONDITION inventory returns scoped asset class/fidelity/source/protected-feature facts and exact counts for profiles, familiarity, views, variants, temporal packages, surface states, anchors, and collection/prefab/kit membership, including kit membership through referenced prefabs.
- Inventory counts expose no collection, prefab, or kit names and do not imply geometry, UV, material, or production readiness.
- A targeted inventory request outside the task scope is rejected; a zero object identity is rejected before consuming query budget.
- A current project sidecar with stale per-asset source revision or digest reports `source_current=false`; stale project revisions fail closed, while unreviewed evidence remains explicitly unreviewed.
- CONDITION tag, familiarity, and inventory asset/source/relationship scans respect the semantic-record budget without overrun; truncation is explicit and inventory rows mark related counts incomplete when the metadata scan did not finish.
- Proximity results report world object-origin distance; orientation results report computed world-space normals; boundary-edge results come from validated mesh topology.
- Missing CONDITION sidecars return an explicit unavailable diagnostic, and CONDITION data with a stale project revision is rejected instead of being shown as current.
- Query count, object scans, topology elements, semantic records, result count, and result-byte limits truncate or reject deterministically without mutating the source.
- CONDITION source lineage and protected-feature strings count toward semantic-record and result-byte budgets.
- Invalid query parameters fail before consuming query budget.
- The UI rejects inventory inspection without a selection; with a selection and no task it runs at the captured live project revision and reports copied facts in `UiSnapshot` without changing project revision or sidecar revision.
- The CONDITION workspace routes selected-asset inspection into Agent, and cached facts disappear from the snapshot when their source revision or selection scope changes.
- After task preparation the UI exposes only the captured task scope; execution rechecks that source revision, and a concurrent human edit rejects execution while leaving the task prepared.

## Capability registry coverage

- Every known native mesh tool has a complete typed contract; kernel/ID drift remains unclassified and cannot be provider-requestable.
- Parameter names match the native tool, numeric bounds are finite and coherent, and length/factor/count units are explicit.
- Geometry-only envelopes expose vertex positioning while excluding topology-changing previews; unsupported registered tools never appear in the planner projection.
- Transform-only projection exposes the typed root-object operation, while the generic AI context remains geometry/topology-only; the translation contract is never auto-approvable.
- The projected context contains no command callback, project document, or direct mutation handle.

## Objective and clarification coverage

- Constraint and question identities are unique, all referenced objects and constraints are in scope, and malformed bounds or candidate answers are rejected.
- A consequential unresolved question blocks task creation without creating or mutating a speculative branch; only the explicit human application boundary can issue an answer receipt, and a selected candidate cannot contradict queued targets.
- The native UI can queue an operator-required question, refuses to run while it is unanswered, and records the explicit answer in the same objective/revision context shown in review.
- The answer receipt is invalidated by a changed plan intent, target, order, or dependency graph, even when the objective text is unchanged.
- Changing the goal or source revision after an answer invalidates the workflow; a failed unresolved attempt does not consume the next task identity.
- Reordering identity lists does not change the canonical objective digest, while changing an answer or hard constraint does.
- Topology-preservation constraints reject a topology capability before speculative execution, and displacement gates block an over-limit result.
- The bounded goal compiler recognizes explicit topology-preservation, world-transform-preservation, and positive vertex-displacement bounds with world-unit suffixes; it rejects malformed or conflicting constraints and direct plans that omit a generated constraint.
- Unsupported restrictions, conditional exceptions, and conflicting topology requirements fail before task creation; the desktop goal editor previews supported limits and disables Run when the goal cannot be enforced.
- UI constraint drafts reject out-of-scope targets, missing rationale, duplicate identities, and unknown constraint kinds; answered objectives cannot be silently weakened, and checkpoint round-trip preserves the accepted constraints.
- A policy-rejected step records the unchanged speculative revision as its after-revision, allowing blocked-task checkpoint validation without inventing a project mutation.
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
- Checkpoint format version 5 persists object targets, transform-distance authority, correction evidence, and recipe identity; it reads versions 1 through 4 while preserving legacy target encoding. Loading remains non-mutating until explicit replay and acceptance.

## Persistent task checkpoint coverage

- Checkpoints are immutable, versioned files with a checksum, source and speculative project digests, objective/plan context digest, work envelope, step progress, failure receipts, repair lineage, last diff digest, validation report, and provider identity. Version 5 round-trips recipe identity; versions 1 through 4 remain readable.
- Checkpoint loading validates size bounds, schema version, project documents, source revision, scope equality, enum values, progress, and digest bindings before exposing workflow state.
- A checkpoint is workflow state. Loading it does not replace or mutate the live project document.
- Human clarification answers and their private receipts are removed from checkpoint plans; consequential questions must be answered again through the human application boundary.
- Same-source resume requires the unchanged source revision and complete serialized source digest, then replays the native plan in a new speculative session. The resulting artifact and diff digests must match before a prior review task can remain acceptable.
- An explicit rebase analysis derives object and mesh read/write sets from objective scope, target objects, mesh bindings, and target ancestry. Object-transform targets do not require a mesh binding, and the transform write set includes the root and every descendant. Disjoint object or mesh edits can be rebased; changed scope/ancestor objects, scoped meshes, target write meshes, or a new out-of-scope instance of a written shared mesh block.
- CONDITION or engineering sidecar changes block rebase while field-level query provenance is unavailable. Rebase requires a plan bound to the exact current revision, unchanged intent/scope/dependencies, and renewed human clarification answers.
- A conflict-free rebase replays the complete native plan against the current source and compares the saved and replayed task write-set objects/meshes. Replay drift blocks acceptance. The accepted result includes all unrelated source edits because the speculative branch starts from the current source.
- Rebase lineage records the previous/current source revisions and digests, canonical read/write set digests, and bounded identities. Checkpoint format version 5 reads versions 1 through 4 without fields introduced by later versions.
- Rebase currently replays the full plan conservatively; per-step invalidation and re-execution from a minimal affected DAG frontier are not implemented.
- Accepted and rejected checkpoints remain inspectable history and cannot be resumed. Checkpoint writes do not replace an existing file, preserving prior task history.
- Corrupted files, attempts to overwrite a checkpoint, conflicting source changes, missing reapproval, and replayed write-set drift are blocked without changing the live project.
- A disjoint source edit can be explicitly rebased, retained in the resulting source, and accepted only after the plan is reauthorized for the new revision.

## Cross-workspace task status

- A review-ready task remains available from other authoring workspaces with passed-step and required-gate counts derived from its current snapshot.
- A blocked task remains visible, exposes its diagnostic on the task action tooltip, and routes back to the Agent workspace; the strip itself does not accept, repair, or mutate the task.
- The strip is hidden in the Agent workspace, after acceptance/rejection, and when no task exists.
- Native-shell smoke coverage should click the global “Ask Cartographer” entry from Project, Market, and Model surfaces, then confirm Agent workspace navigation and goal-field focus without changing task or project state.

## Recipe contract coverage

- `Bounded mesh edit` keeps the operator-authored plan and binds its recipe ID into objective and clarification digests; changing a recipe after a human answer is rejected until the objective is reauthorized.
- The recipe clamps topology work to 512 operations, rejects transform authority, and adds a required scene-identity gate that blocks object add/remove, renaming, transform, hierarchy, visibility, lock, and mesh-binding changes.
- Checkpoint format v5 round-trips the recipe; older versions remain readable with no recipe selected. A forged checkpoint that expands the recipe envelope is rejected.
- `Topology-preserving vertex edit` limits the work envelope to geometry effects and zero topology operations. It adds a required gate over mesh, vertex, and face topology changes while allowing vertex-position edits; topology-changing operations and transform authority fail closed, and repair reapplies the recipe limits.
- A forged checkpoint that adds topology permission to the vertex recipe is rejected, while a geometry-only edit and a repaired geometry-only plan remain reviewable without mutating the source project.
- Recipes constrain validation and envelope policy; they do not generate steps or infer an objective.


## Recorded demonstration coverage

- Only a human-accepted task with passed steps and all required gates can be captured; review-ready and blocked tasks are rejected.
- Serialization round-trips bounded step/gate evidence, checksums, and relative revision offsets; corrupted content, invalid aliases, and failed required gates are rejected.
- Format v2 records normalized face-operation distances and relative vertex/object displacements, rejects tool/parameter mismatches and invalid numeric values, and survives checkpoint replay; format v1 records remain readable as inspection-only evidence without typed parameters.
- Vertex-position traces store the delta from the pre-step vertex instead of its absolute position; face distances are divided by the selected face's longest edge before they leave the branch.
- The record excludes project bytes, real object IDs, object names, goal/step text, prompts, answers, and paths; step object aliases preserve only within-record relationships.
- Saving is immutable and size-bounded. Opening a record does not create an AgentTask, mutate a project, or cross the human acceptance boundary.
- The format checksum detects accidental edits only; it is not a signature or proof that the trace came from a trusted Cartographer process.
- Human-authored trace capture accepts only a contiguous recent range of supported native operations (face inset/extrude, vertex-position change, and root-object translation); AI proposals and unsupported project-changing edits are not admitted as human trace steps.
- The in-memory trace is bounded to 256 operations, revision-contiguous, and cleared by unsupported project changes. Export assigns object/component aliases and persists only normalized face distance or relative displacement parameters.
- Manual trace serialization rejects malformed aliases, non-finite parameters, invalid ranges, oversized records, and checksum corruption; saving is immutable, loading is read-only, and capture/load do not change the source project revision.
- The UI binds every trace alias to exactly one selected target of the recorded object/face/vertex kind. It rejects unknown aliases, wrong selection kinds, multiple source aliases mapped to one target object, inconsistent component bindings, and bindings made stale by a project revision change.
- Preview scales normalized face distances by the selected target face's longest boundary edge and applies vertex deltas to the selected target vertex. Object, face, and vertex parameters become a sequential bounded AgentTask; repeated vertex edits are allowed only for one aliased component with stable pre-component and post-object signatures, and their deltas accumulate on the speculative branch. Shared mesh assets across target sets remain excluded.
- A successful bound trace preview produces a review task and diff without changing the source revision. The existing explicit acceptance action is required to commit; acceptance then invalidates the old target bindings by revision.
- Trace v2 preserves the existing alias-only privacy boundary and adds topology-only before-command and post-command object signatures. It contains no source object/component IDs, names, coordinates, transforms, target bindings, acceptance gates, or source authority. Trace v1 remains readable without correspondence evidence.
- “Apply to similar” supports any one captured command and verifies the selected completed exemplar against its post-command structure. It excludes that exemplar, finds pre-command object/component signatures, skips per-object component ambiguity, omits repeated mesh assets for geometry edits, and caps target sets at 16. The result remains a speculative preview and does not accept automatically.
- Multi-command matching is enabled for root-object translations to one alias with unchanged structural signatures, and repeated vertex-position changes to one object/component alias when all pre-component signatures and post-object signatures stay stable. A mismatched signature is rejected before bindings are created; the accepted preview retains the full ordered step chain and leaves the source revision unchanged. Multi-command topology-changing geometry edits remain unsupported.
- Structural correspondence tests cover two unique signature matches, a different-topology distractor, the wrong exemplar, zero matches, symmetric ambiguous faces, shared-mesh exclusion, signature round-trip, privacy canaries, and unchanged source revision through preview.

## Follow-up coverage

Multi-target demonstration tranche:

- “Apply to selection” creates 1–16 explicit target sets, requires every alias in every set to be bound, and clears all bindings when the set count changes.
- A source object cannot be reused across sets, and preview rejects hierarchy-expanded scope overlap.
- Multi-set preview expands each trace into an independent ordered dependency chain in one DAG, keeps the 256-step plan cap, scales command, topology, and transform budgets across sets, and rejects missing bindings without opening a task.
- A two-set preview covers six expanded steps, applies accepted transforms to both target sets, and leaves the source revision unchanged until acceptance. Changing set count during a task is rejected.
- A demonstration recipe round-trips its bounded user-selected name and redacted trace, detects corruption, and rejects control characters in the name. It omits object-name canaries from the trace payload.
- Recipe files are immutable. Opening a recipe leaves the project revision unchanged, creates no task, carries no target bindings, and cannot preview until current targets are explicitly rebound.
- Trace v1 payloads remain readable after the v2 structural signature extension. Signatures are stable across object names, IDs, scale, and translation because they encode only bounded topology counts and local incidence.

Add failure injection for durable journal append rollback, additional unrelated and conflicting source edits during safe rebase, bounded repair exhaustion, semantic-sidecar size pressure, and provider timeouts as those tranches are implemented. Clarification questions and answers are reviewable evidence, not natural-language commands; the workbench compiles only explicit topology, transform, and numeric displacement constraints and does not infer questions automatically.
