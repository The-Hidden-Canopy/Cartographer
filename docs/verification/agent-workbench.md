# Agent Workbench verification contract

## End-to-end fixture

- The UV workspace opens a dedicated UV authoring instrument, retains the current mesh selection, and offers bounded lightmap-candidate inputs. Drafting opens the Agent review flow; the plan can run only for multiple unique mesh assets in a complete unlocked scope, and stays speculative until explicit acceptance.

- Create a project with separate mesh objects.
- Capture exact face targets and queue native inset/extrude steps plus a parameterless face poke.
- Run the plan on the speculative session.
- Confirm the source revision and geometry remain unchanged before review.
- Confirm per-step receipts, project/viewport gates, and structured mesh and material-region binding diffs.
- Material assignment, reassignment, and removal appear in review with before/after material and variant identities; material source edits, texture-source content/metadata changes, and texture dependency changes have separate evidence rows. A material-only change satisfies the non-empty diff predicate.
- The Agent Workbench material permission is off by default. With it enabled, `assign material <id> to region <id> [variant <id>]` runs only on the speculative branch and requires exact non-zero unsigned IDs.
- The Material workspace can draft the selected material/region assignment into the Agent Workbench for 1–16 unlocked root mesh objects only when every instance of each target shared mesh is selected; pending Material edits, an existing Agent draft/task/checkpoint, an invalid material, or region zero disable the entry action. The action compiles a plan and enables the material effect, then routes to Agent without changing the source.
- The Material workspace UV form requires one to sixteen unlocked root objects with unique mesh assets, a non-zero candidate set ID, bounded resolution/padding/tiles, and an idle Agent draft state. It compiles the explicit UV intent, grants the UV effect, and routes to Agent; the candidate remains speculative and no seam, bake-freshness, or readiness claim is shown. A multi-object unique-mesh fixture reviews and accepts the candidate on both meshes while preserving the source until acceptance.
- If several selected objects share one mesh and request the same assignment, the later step passes as a no-op only after an earlier passed step in the same task wrote the same mesh/region/material/variant value. A source assignment that was already equal does not gain this shortcut. Checkpoint save/load/resume preserves both step receipts and remains review-only until explicit acceptance.
- Confirm material-only and mixed geometry/material plans remain review-only, report a material-binding diff, preserve the source before acceptance, and commit through the normal human acceptance event.
- Confirm omitted material permission, malformed or overflowing IDs, missing materials, missing variants, undeclared regions, and shared mesh instances outside scope block without changing the source.
- The Agent Workbench UV permission is off by default. With it enabled, `generate lightmap uv set <id> resolution <pixels> padding <texels> tiles <count>` adds only a new, non-active lightmap-candidate set and corner layer; the existing UV sets and topology remain unchanged.
- Confirm UV review reports the new descriptor, exact changed-corner count and bounded samples, leaves the source revision and journal unchanged, and remains review-only after checkpoint save/resume. Acceptance writes one journal event; save/reopen, undo, and redo preserve or remove the candidate set as expected.
- Confirm omitted UV permission, zero/overflowing IDs, invalid resolution/padding/tile bounds, stale source revision, occupied UV-set/layer identities, unsupported planar geometry, unfit atlas budgets, and shared-mesh instances outside scope fail without changing the source.
- Confirm generic AI cannot enter the UV preview or auto-apply path, and UV generation is projected only when the Agent Workbench envelope explicitly grants the UV effect.
- Confirm generic AI proposals cannot request the material capability, and material checkpoints replay only against their exact source revision; rebase blocks while catalog writes are absent from its read/write-set model.
- Expand changed meshes and inspect all changed vertex positions and face-connectivity identities, including added/removed faces; large lists must remain scrollable without silently truncating evidence.
- Accept the result and confirm the source advances once and the saved-project journal contains one application mutation event.
- From Model, Material, CONDITION, and Review, use the global Ask Cartographer entry and confirm the Agent Workbench instrument opens with the goal field focused while preserving the selected object or component and source revision.
- From Sculpt, use the selected-target handoff and confirm the Agent workspace opens with the goal field focused, the selected target and source revision unchanged, and no plan/task created; the UI must state that SculptStack layers, masks, remesh, and multires settings are not Agent operations.
- From a Scene outliner row, use the context menu to ask Cartographer about that object; confirm the row's object becomes the sole selected target and no plan or task starts automatically.
- From a review-ready persistent task strip, choose Review result and confirm the Review workspace opens the Agent Workbench with the speculative after branch visible.

## Adversarial boundaries

- A target outside objective scope is rejected before execution.
- A protected target is rejected before execution.
- A target mesh shared by an out-of-scope or protected instance is rejected, and all in-scope instances count toward the touched-object budget.
- A topology edit is blocked when the envelope permits geometry only.
- Face poke is blocked by geometry-only scope, rejects numeric parameters, stays off the live source before review, and remains unavailable to AI auto-apply.
- A human-accepted face-poke step serializes as a parameterless typed demonstration; older demonstration versions remain readable.
- A plan based on a stale source revision cannot start.
- A missing goal cannot start a task, and a source edit after speculative execution prevents acceptance without overwriting the newer source.
- An invalid native operation blocks the task without changing the source.
- Acceptance before gates pass, repeated execution, or rejection after acceptance is invalid.
- The UI captures only a single exact object, face, or vertex per step and exposes explicit accept/reject actions.
- `on object "Exact Name": command` resolves through the bounded current-project query; a unique match overrides selection, while duplicate names require exactly one matching unlocked root selected. The adversarial 17-match fixture proves a large bounded duplicate set still resolves to one explicit target, and only that identity enters the compiled target and work-envelope scope. Missing, unresolved, stale, incomplete, child, and locked targets fail without installing a plan or changing the source.
- `on nearest object within <radius> world units of "Anchor": command` resolves the uniquely nearest non-anchor object origin within the positive bounded radius, ignores viewport-selection fallback, and installs only that root in plan scope. Parser bounds, a unique-nearest speculative acceptance path, duplicate/missing anchor rejection, out-of-radius rejection, and equidistant-candidate rejection are covered; an incomplete query or locked/nested nearest candidate must fail without substituting a farther object.
- Named-target parsing decodes only `\"` and `\\`, rejects malformed quoting/escaping and empty command text, and excludes the target name from hard-constraint parsing.
- Object translation is absent from the generic AI proposal path, requires an explicit Agent Workbench transform permission and positive cumulative meter bound, and cannot change the source before human acceptance.
- Translation rejects multiple selected objects, missing transform permission, an invalid or exceeded distance bound, and an objective `preserve_transform` constraint; a successful root translation changes the object transform while preserving rotation, scale, and mesh content.
- Distinct geometry results produce distinct digests even when their mesh counts and revision shape match.

## SceneQuery coverage

- Exact object ID, stable name, and mesh-asset queries return copied identities at the source revision.
- Name, spatial, topology, and CONDITION results are limited to the task scope; an explicit out-of-scope identity request fails closed.
- CONDITION tag and familiarity facts include their source ID, revision, current/reviewed flags, and protected features where applicable; stale or unreviewed source evidence is never projected as current.
- CONDITION inventory returns scoped asset class/fidelity/source/protected-feature facts and exact counts for profiles, familiarity, views, variants, temporal packages, surface states, anchors, and collection/prefab/kit membership, including kit membership through referenced prefabs.
- Inventory counts expose no collection, prefab, or kit names and do not imply geometry, UV, material, or production readiness.
- The desktop evidence audit counts only current/reviewed source lineage and complete metadata scans; stale, unreviewed, malformed-lineage, empty, diagnostic, or truncated input never establishes readiness.
- The audit explicitly keeps production readiness false because inventory facts do not evaluate geometry, UV/material state, collision, or domain-specific validation gates.
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
- Scoped CONDITION inventory displays bounded mesh validation, active render/lightmap UV-set presence, and explicit material-region assignment resolution. Unassigned regions and missing lightmap candidates fail closed; topology-budget exhaustion remains incomplete, and no technical result claims overall readiness.
- The CONDITION audit reports explicit per-gate pass/fail/incomplete/unavailable states for source lineage, metadata-scan completeness, mesh validation, render-UV presence, and material-region resolution. Each gate distinguishes its observed asset scope from asset rows with failed, unavailable, or incomplete evidence; unsupported checks have scope but no invented affected assets. Truncated, mixed, empty, malformed, or stale evidence cannot produce a complete observed-check pass; unsupported tangent, bake, collision, traversal, anchor-validity, source-digest, and domain-policy gates remain visibly not evaluated, and overall readiness remains false.
- After task preparation the UI exposes only the captured task scope; execution rechecks that source revision, and a concurrent human edit rejects execution while leaving the task prepared.

## Capability registry coverage

- Every known native mesh tool has a complete typed contract; kernel/ID drift remains unclassified and cannot be provider-requestable.
- Parameter names match the native tool, numeric bounds are finite and coherent, and length/factor/count units are explicit.
- Geometry-only envelopes expose vertex positioning while excluding topology-changing previews; unsupported registered tools never appear in the planner projection.
- Transform-only projection exposes the typed root-object operation, while the generic AI context remains geometry/topology-only; the translation contract is never auto-approvable.
- UV-only projection exposes exactly the typed lightmap-generation operation; its unsigned set identity, resolution, padding, and tile bounds agree with preview validation, and the operation is never auto-approvable.
- The projected context contains no command callback, project document, or direct mutation handle.

## Objective and clarification coverage

- Constraint and question identities are unique, all referenced objects and constraints are in scope, and malformed bounds or candidate answers are rejected.
- A consequential unresolved question blocks task creation without creating or mutating a speculative branch; only the explicit human application boundary can issue an answer receipt, and a selected candidate cannot contradict queued targets.
- The native UI can queue an operator-required question, refuses to run while it is unanswered, and records the explicit answer in the same objective/revision context shown in review.
- The desktop disables question submission until a bounded operator rationale is provided; controller validation rejects control-character and over-limit text, and the accepted rationale is bound into the clarification context and shown before an answer.
- The UI can show two or more authored candidate choices, records only a listed choice through the human application boundary, rejects unknown choices and out-of-scope candidate targets, and preserves the source revision until explicit acceptance.
- Before an answer, the desktop clarification panel shows the operator rationale, consequence categories, affected-object count, and downstream branch count; expandable rows identify question and candidate object-scope mappings.
- Candidate answers add no plan steps or scope; a changed plan invalidates the receipt and requires a new operator answer.
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
- The UI can compose a face edit on one object and a root transform on another under an explicit combined envelope; review shows the mesh and transform result while the source remains unchanged until one human acceptance.
- ProjectDiff compares attribute-layer descriptors and values, binds UV sets to stable corner identities, reports exact changed-corner counts, and caps displayed UV samples at 256 per set without truncating the total count.
- Goal compilation over multiple selected root objects creates one independent branch per object and chains `then` commands only within each branch; review and explicit acceptance apply the whole candidate once.
- Locked or nested object selections and multi-target command expansion beyond the 16-step work envelope fail atomically without installing a plan or changing the source.
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
- Transform corrections require an in-scope, unprotected, unlocked root object; vertex and face corrections require an in-scope, unprotected, unlocked mesh target and every instance of its shared mesh inside scope. Face inset/extrusion additionally requires a finite positive distance, geometry and topology effects, and no applicable preserve-topology constraint. Out-of-scope, non-finite, empty-reason, missing-component, forbidden-effect, and stale-source attempts leave the speculative revision unchanged.
- A correction mutates only the speculative session, refreshes the ProjectDiff and rendered after-scene, and re-runs acceptance gates. Acceptance remains blocked when any required gate fails.
- Transform receipts bind the task identity, object, before/after transforms, source and branch revisions, plan context, parent evidence digest, and operator reason. Vertex receipts bind the object and vertex, before/after local positions, before/after canonical mesh digests, and the same revision and lineage data. Face receipts bind the object and face, native inset/extrusion kind, distance, before/after canonical mesh digests, and the same revision and lineage data.
- Repair, same-source resume, and disjoint-source rebase replay locked transforms, vertex edits, and face inset/extrusion only when the replayed branch matches the recorded before value or mesh digest and reproduces the recorded after mesh digest. Repair lineage binds the inherited correction chain; replay drift blocks acceptance.
- Anchored corrections replay immediately after the last completed plan step that wrote the corrected target; its transitive dependent steps then run again in topological order. Corrections without an earlier writer replay after the plan. The recompute receipt preserves superseded step/gate evidence, dependent IDs, before/after diff digests and revisions, and replay status.
- A forged or missing anchor, exhausted recompute budget, stale correction digest, or replay drift blocks the speculative correction path. The source remains unchanged.
- Checkpoint format version 11 persists typed transform, vertex, and face-topology correction evidence, recompute anchors and receipts, rebase reuse IDs, material-region read/write keys, consumed SceneQuery budget counters, recompute budget, object/component targets, transform-distance authority, and recipe identity. It reads versions 1 through 10 while preserving their prior field layout; older versions have no persisted query-usage counters. Loading remains non-mutating until explicit replay and acceptance.
- Sequential face inset and extrusion receipts preserve a continuous digest chain through checkpoint save/load and disjoint-source rebase; forged distance evidence is rejected. A geometry-only recipe rejects topology ghost correction before the speculative revision advances.
- A vertex correction anchored after step 1 reruns its dependent step 2 against the corrected branch, records the prior receipts, survives checkpoint round-trip and resume, and leaves the source unchanged. A forged anchor and a zero recompute budget are rejected.

## Persistent task checkpoint coverage

- Checkpoints are immutable, versioned files with a checksum, source and speculative project digests, objective/plan context digest, work envelope, step progress, failure receipts, repair lineage, rebase reuse lineage, ghost recomputation lineage, last diff digest, validation report, and provider identity. Version 11 adds consumed SceneQuery counters bound to the envelope limits; version 10 adds read/write material-region keys to rebase receipts; version 9 adds reused step IDs; version 8 stores typed vertex and face ghost corrections plus recompute anchors/receipts. Versions 1 through 10 remain readable.
- SceneQuery budget usage survives checkpoint save, load, same-source resume, and safe rebase; a checksummed checkpoint whose usage exceeds its limits is rejected. Legacy versions default usage to zero because they did not persist counters.
- Checkpoint loading validates size bounds, schema version, project documents, source revision, scope equality, enum values, progress, and digest bindings before exposing workflow state.
- A checkpoint is workflow state. Loading it does not replace or mutate the live project document.
- Human clarification answers and their private receipts are removed from checkpoint plans; consequential questions must be answered again through the human application boundary.
- Same-source resume requires the unchanged source revision and complete serialized source digest, then replays the native plan in a new speculative session. The resulting artifact and diff digests must match before a prior review task can remain acceptable.
- An explicit rebase analysis derives object and mesh read/write sets from objective scope, target objects, mesh bindings, and target ancestry. Object-transform targets do not require a mesh binding, and the transform write set includes the root and every descendant. Disjoint object or mesh edits can be rebased; changed scope/ancestor objects, scoped meshes, target write meshes, or a new out-of-scope instance of a written shared mesh block.
- CONDITION or engineering sidecar changes block rebase while field-level query provenance is unavailable. Rebase requires a plan bound to the exact current revision, unchanged intent/scope/dependencies, and renewed human clarification answers.
- For a review checkpoint with no invalidated plan steps, rebase replays the checkpoint against its exact saved source and requires the complete speculative artifact and diff digests to match. It then validates that the artifact changed only declared scene, mesh, and material-region writes, merges those writes onto the current document, preserves unrelated source edits and topology history, re-evaluates acceptance gates, and records reused step IDs. Material-region writes are keyed by mesh asset and region; changes at other region keys are preserved, while a changed written key or material source definition blocks rebase. The branch is installed through the speculative command path; it is not accepted into the live project.
- When a read-only object input changes, rebase re-executes the directly affected steps and their dependents in scheduler order. Review checkpoints with unique, disjoint per-step writes reuse verified unaffected outputs at their plan position; overlapping write sets fall back to full native replay. A changed task write object or mesh, shared-mesh membership change, or semantic sidecar change blocks rebase.
- A transform change on a geometry step's target is a read dependency rather than a task object write: it invalidates that step, keeps the human transform on the current source, and replays the mesh step against it. External edits to the written mesh itself remain conflicts.
- Rebase lineage records the previous/current source revisions and digests, canonical read/write set digests, bounded object, mesh, and material-region identities, and any reused step IDs. Checkpoint format version 10 reads versions 1 through 9 without the material-region keys.
- Rebase analysis reports directly affected steps and their transitive dependency closure in stable scheduler order. The Agent workspace shows both lists. A read-only source change can proceed when its replayed writes remain disjoint; unaffected steps are reused only when their write resources are unique and disjoint. Plans with material assignment steps use complete replay when any step is invalidated. Checkpoints with ghost corrections and plans with repeated step writes also use complete replay.
- Material-region checkpoint tests cover rebase over a disjoint scene edit, round-trip of region keys in rebase lineage, preservation of an external assignment at another region, rejection of an assignment to the same region, and rejection after material source definition changes. All rebased assignments remain speculative until explicit acceptance.
- The Agent workspace displays each rebase receipt's source revisions and bounded material-region read/write keys; the UI round-trip test confirms those keys survive checkpoint resume and remain visible in the task snapshot before acceptance.
- The adversarial frontier test changes the target transform for `step-a`, invalidates dependent `step-b`, re-executes both against the current source, and reuses independent `step-c`. It verifies scheduler-order revisions, unchanged live source before acceptance, exact checkpoint resume after save/load, and accepted preservation of the external transform. A separate test confirms a changed object-transform write target still blocks.
- Accepted and rejected checkpoints remain inspectable history and cannot be resumed. Checkpoint writes do not replace an existing file, preserving prior task history.
- Corrupted files, checksummed but tampered speculative artifacts, attempts to overwrite a checkpoint, conflicting source changes, missing reapproval, and replayed write-set drift are blocked without changing the live project.
- A disjoint source edit can be explicitly rebased, retained in the resulting source, and accepted only after the plan is reauthorized for the new revision.
- An adversarial DAG fixture changes one target source object, verifies that only its direct step and transitive dependent step are reported invalidated, excludes the independent step, and confirms the UI exposes the same evidence without mutating the live source.

## Cross-workspace task status

- A review-ready task remains available from other authoring workspaces with passed-step and required-gate counts derived from its current snapshot.
- A blocked task remains visible, exposes its diagnostic on the task action tooltip, and routes back to the Agent workspace; the strip itself does not accept, repair, or mutate the task.
- The strip is hidden in the Agent workspace, after acceptance/rejection, and when no task exists.
- Native-shell smoke coverage should click the global “Ask Cartographer” entry from Project, Market, and Model surfaces, then confirm Agent workspace navigation and goal-field focus without changing task or project state.

## Goal-to-plan compilation

- The goal editor compiles supported native command clauses separated by case-insensitive `then` or semicolons into a linear plan DAG bound to the one selected object, face, or vertex and the captured source revision.
- Compilation validates each clause against the native preview ontology and installs no partial plan when a clause is unsupported. The source revision remains unchanged; task preparation and execution still require their existing explicit UI actions.
- The sequence limit is 16 commands. Extra instruction text is rejected by the native planner rather than silently ignored, and an existing manual draft cannot be replaced by compilation.
- An end-to-end controller fixture compiles two object translations, checks their dependency edge, prepares and runs the task speculatively, verifies the live source remains unchanged before acceptance, then accepts the result through the human application boundary.
- `on CONDITION tag "Exact Tag":` resolves exact tag membership from the current project sidecar, creates one deterministic chain per unique authored root, and runs through speculative review and explicit acceptance. It never falls back to current selection; missing sidecars, unknown tags, stale or unreviewed source evidence, protected features, malformed facts, incomplete queries, and target/plan budget overflow install no partial draft.

## Recipe contract coverage

- `Bounded mesh edit` keeps the operator-authored plan and binds its recipe ID into objective and clarification digests; changing a recipe after a human answer is rejected until the objective is reauthorized.
- The recipe clamps topology work to 512 operations, rejects transform authority, and adds a required scene-identity gate that blocks object add/remove, renaming, transform, hierarchy, visibility, lock, and mesh-binding changes.
- Checkpoint format v7 round-trips recipes and typed vertex/face correction evidence; older versions remain readable with no recipe selected. A forged checkpoint that expands the recipe envelope is rejected.
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

Agent acceptance now injects a durable journal append failure and verifies rollback of the live project, command history, saved project bytes, and journal bytes; recovery remains clean with the original entry count. Rebase also combines a changed read target with an unrelated out-of-scope edit and verifies that replay plus acceptance preserves both source transforms while applying the planned mesh edit. Semantic-sidecar size pressure and provider timeouts remain failure-injection follow-ups as those surfaces are implemented. Clarification questions and answers are reviewable evidence, not natural-language commands; the workbench compiles only explicit topology, transform, and numeric displacement constraints and does not infer questions automatically.
