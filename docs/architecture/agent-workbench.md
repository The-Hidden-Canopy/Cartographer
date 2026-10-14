# Agent Workbench: speculative authoring slice

## Capability

Cartographer can capture a goal and a bounded sequence of exact face or vertex operations, execute the native proposals on a private copy of the current project, validate the branch, and present a structured object/mesh diff for review. The source project stays unchanged until a user accepts the result. The task also exposes bounded read-only queries over the branch for scoped object identity, exact names, mesh instances, object-origin proximity, face orientation, boundary edges, CONDITION tags, and familiarity records.

The initial native planner supports face inset, face extrusion, and vertex positioning. It does not call an external model or infer targets. The user captures each target from the normal viewport selection path.

## Authority path

```text
captured targets + goal
    -> AgentPlan + AgentWorkEnvelope
    -> private ApplicationSession copy
    -> scoped SceneQuery facts + read-budget ledger
    -> NativePlanner proposal
    -> revision-bound AuthoringPreview
    -> native application command on the copy
    -> project / viewport / plan / diff gates
    -> explicit user accept
    -> source revision check + application journal event
```

The speculative application copy has no project path or journal binding. It cannot save, publish, or mutate the source. Acceptance rejects a stale source revision, validates the complete candidate, resets the candidate's project revision to one step after the source, and writes one application mutation snapshot with the task ID and diff digest before publishing it in memory. A failed journal append restores the source document. The accepted snapshot is one undoable application command; because it replaces complete project state, prior command undo/redo history is replaced by that single result command.

## Work envelope

The envelope limits commands to captured object IDs, geometry/topology effects, at most 16 commands, and a bounded touched-object count. Read budgets independently cap query count, objects examined, topology elements examined, semantic records examined, and result bytes. Repair budgets cap retries and additional operations. Queries return copied facts at a source revision and filter results to the objective scope. Protected object IDs are rejected during plan validation. Because Cartographer can share one mesh asset across several scene objects, every instance of a target mesh must also be inside scope and unprotected; the touch budget counts all affected instances. The planner only emits registered typed proposals, and each proposal must pass its current native ontology and preview validation.

The native capability registry describes every registered mesh command with typed parameter bounds and units, target/read/write shapes, effects, invalidations, validators, reversibility risk, and advisory cost hints. Only three commands currently have native preview contracts (face inset, face extrusion, and vertex positioning). The workbench projects that registry through the task's effect mask and exposes only previewable capabilities whose effects fit the envelope. Cost hints describe expected work; query and touch limits remain the enforced budgets. A registry entry never contains a command callback or a document reference.

Objectives can carry hard constraints for preserving topology, preserving an object's world transform, or limiting world-space vertex displacement. Displacement gates compare every stable vertex correspondence, include object world transforms, and fail closed when topology cannot be matched or the measurement budget is exceeded. Constraint gates block acceptance, and topology preservation rejects a conflicting command before it runs on the speculative branch. A deterministic clarification frontier ranks declared questions by the consequences they affect and the number of downstream branches. High-impact unresolved questions block task creation; questions explicitly marked as operator-required also block regardless of score. Candidate choices are checked against queued targets. The native workbench can collect an operator-authored question and record its answer against the exact goal, target scope, objective revision, question context, and ordered plan steps/dependencies. Any changed plan invalidates the answer receipt. Answers appear in the review snapshot and acceptance digest. The system still does not infer objective meaning or generate questions from free-form goals.

Plan steps may depend on earlier captured steps. Validation rejects missing, duplicate, self, and cyclic dependencies. A stable topological scheduler executes one native command at a time within the command budget. When a step fails, its dependent branch is marked invalidated while independent ready steps may continue on the private project copy; any failed or invalidated step blocks acceptance. A blocked attempt can be repaired only with an operator justification and a revised plan that preserves the objective context and object scope while keeping every envelope limit the same or narrower. Repair always starts from a fresh copy of the unchanged source. It retains the earlier step receipts, gate results, and evidence digests in the repair chain. Changing a plan invalidates its human clarification receipts; the operator must answer again against the repaired plan before execution. Retry and added-operation budgets are cumulative across the repair chain. The Agent workspace can select and revise a same-object step, re-answer invalidated questions, and shows the final branch state and prior failure evidence.

## Review evidence

The review view includes source/before/after viewport modes, per-step receipts, required gates, changed object and mesh records with vertex positions and face connectivity, changed engineering/CONDITION sidecar flags, and a SHA-256 digest of the complete serialized branch result. Every gate also carries its validator version, source and evaluated revisions, affected objects and assets, result digest, canonical evidence digest, and any diagnostic. The digest binds the gate outcome and evidence to those identities and revisions. Branch viewport modes are read-only and disable source selection. Required gates are project validity, viewport compilation, all plan steps passing, and a non-empty diff. Whole-result acceptance is explicit.

## Deliberate limits

SceneQuery is intentionally a small typed vocabulary. It does not expose collection/prefab lookup, viewport visibility, UV seams, material regions, non-manifold areas, navigation/readiness diagnostics, arbitrary predicates, or provider-side document access. Query results are read-only and revision-bound; missing CONDITION data is reported as unavailable, and stale CONDITION revisions fail closed. Repair is operator-directed replanning, not autonomous model repair: the native workbench does not infer a correction, add scope, or relax constraints. The remaining docuseries still needs automatic objective compilation and question generation, broad multi-domain capability composition, persistent tasks, recipes, demonstration, ghost corrections, safe rebase, and integration across all authoring workspaces. Tasks fail closed if the project revision changes before acceptance.
