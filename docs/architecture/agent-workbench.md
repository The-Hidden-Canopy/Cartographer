# Agent Workbench: speculative authoring slice

## Capability

Cartographer can capture a goal and a bounded sequence of exact face or vertex operations, execute the native proposals on a private copy of the current project, validate the branch, and present a structured object/mesh diff for review. The source project stays unchanged until a user accepts the result.

The initial native planner supports face inset, face extrusion, and vertex positioning. It does not call an external model or infer targets. The user captures each target from the normal viewport selection path.

## Authority path

```text
captured targets + goal
    -> AgentPlan + AgentWorkEnvelope
    -> private ApplicationSession copy
    -> NativePlanner proposal
    -> revision-bound AuthoringPreview
    -> native application command on the copy
    -> project / viewport / plan / diff gates
    -> explicit user accept
    -> source revision check + application journal event
```

The speculative application copy has no project path or journal binding. It cannot save, publish, or mutate the source. Acceptance rejects a stale source revision, validates the complete candidate, resets the candidate's project revision to one step after the source, and writes one application mutation snapshot with the task ID and diff digest before publishing it in memory. A failed journal append restores the source document. The accepted snapshot is one undoable application command; because it replaces complete project state, prior command undo/redo history is replaced by that single result command.

## Work envelope

The first envelope limits commands to captured object IDs, geometry/topology effects, at most 16 commands, and a bounded touched-object count. Protected object IDs are rejected during plan validation. Because Cartographer can share one mesh asset across several scene objects, every instance of a target mesh must also be inside scope and unprotected; the touch budget counts all affected instances. The planner only emits registered typed proposals, and each proposal must pass its current native ontology and preview validation.

## Review evidence

The review view includes source/before/after viewport modes, per-step receipts, required gates, changed object and mesh records with vertex positions and face connectivity, changed engineering/CONDITION sidecar flags, and a SHA-256 digest of the complete serialized branch result. Branch viewport modes are read-only and disable source selection. Required gates are project validity, viewport compilation, all plan steps passing, and a non-empty diff. Whole-result acceptance is explicit.

## Deliberate limits

This slice does not yet implement SceneQuery, objective compilation beyond goal/scope/protected IDs, plan DAG scheduling, model-backed planning, read/write-set rebase, self-repair, persistent tasks, recipe execution, teach-by-demonstration, user edits to a ghost branch, partial acceptance, or viewport ghost overlays. Tasks fail closed if the project revision changes before acceptance.
