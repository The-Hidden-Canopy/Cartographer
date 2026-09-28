# Architecture overview

Cartographer is a local-first authoring application with one durable source of
truth and several derived layers. The core data path is:

```text
tool input
  -> ephemeral selection/tool adapter
  -> registered tool action
  -> validated editor command
  -> scene/project authoring state
  -> geometry validation
  -> deterministic compiled mesh + revision
  -> render snapshot / interchange output
```

## Ownership

| Layer | Owns | Must not own |
| --- | --- | --- |
| `carto_core` | diagnostics, revisions, precision math | scene or UI policy |
| `carto_scene` | stable object identity, local transforms, parent links | renderer resources |
| `carto_geometry` | editable mesh topology and derived compilation | UI state or GPU handles |
| `carto_project` | versioned authoring persistence and asset references | render caches |
| `carto_editor` | commands and undo/redo history | direct panel-owned mutations |
| `carto_render` | compiled render snapshots | editable topology |
| `carto_io` | format adapters and loss reports | project model ownership |
| future application shell | panes, workspaces, actions | authoritative model state |

## Failure posture

Rejected inputs return a `Diagnostic` with a stable error category. The 0.1
implementation rejects malformed topology, hierarchy cycles, dangling
references, stale render revisions, unsupported project schema versions,
invalid project-relative asset paths, and failed atomic writes. It does not
silently repair or flatten these cases.

Primitive constructors are deterministic input tools rather than a second
source of truth: they generate ordinary editable mesh authoring data. Vertex
position edits validate the whole mesh before committing a revision; a failed
edit restores the prior position and cannot enter command history.

Single-face extrusion removes the source face, creates the side walls and cap,
then validates the complete mesh. Any failed generated vertex or face restores
the pre-operation mesh before returning a diagnostic; its editor command keeps
validated before/after topology for reversible history.

Selection state is an ephemeral editor concern. Object, vertex, and face
selections are isolated by mode, are never serialized into project truth, and
must be validated against the current scene or mesh before a tool consumes
them. Edge mode remains deferred until the topology layer has stable edge
identity rather than snapshot-only half-edge IDs.

Tool actions are registered by stable IDs and return editor commands. The
registry submits successful commands through `CommandBus`; it does not expose a
direct mutation callback or mutable mesh accessor to panels or UI code. The
current built-in context exposes an approved command builder for selected-face
extrusion; additional tool builders must preserve the same boundary.

Asynchronous jobs and GPU resources are not implemented yet. When they are
introduced, publication must verify the source revision before replacing a
derived result. That rule is already represented by `CompiledMesh::source_revision`
and `RenderScene::upsert`.
