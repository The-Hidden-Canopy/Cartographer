# Architecture overview

Cartographer is a local-first authoring application with one durable source of
truth and several derived layers. The core data path is:

```text
tool input
  -> application action/snapshot boundary
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
| `carto_application` | front-end actions, workspace state, immutable snapshots | direct panel-owned mutations or GPU resources |
| optional native shell | Win32 windowing, ImGui panels, Vulkan submission | authoritative model state or editable topology |

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

Project-bound editor commands also carry the mesh revision they last applied.
Undo and redo use a revision-conditional replacement, so an external project
edit causes a stale-data failure instead of being silently overwritten.

Selection state is an ephemeral editor concern. Object, vertex, and face
selections are isolated by mode, are never serialized into project truth, and
must be validated against the current scene or mesh before a tool consumes
them. Component selections may be bound to one scene object; add/toggle
operations cannot span objects or silently bind an existing unbound selection
to a different mesh context. Mesh-bound selections retain the mesh instance and
source revision they were created against, so replacement or mutation requires
explicit reselection. Edge mode remains deferred until the topology layer has
stable edge identity rather than snapshot-only half-edge IDs.

Tool actions are registered by stable IDs and return editor commands. The
registry submits successful commands through `CommandBus`; it does not expose a
direct mutation callback or mutable mesh accessor to panels or UI code. The
current built-in context resolves an object-bound selection through
`ProjectDocument` to the owning mesh asset and exposes approved command
builders for selected-face extrusion and single-vertex position editing.
Additional tool builders must preserve the same boundary and leave context
routing out of panel code.

## Application boundary

`carto_application::ApplicationSession` is the first front-end integration
layer. It owns the active project document, project path and saved revision,
ephemeral selection, command history, tool registry, validated workspace pane
state, and user-visible problems. Its action variant covers the 0.1 vertical
slice: new/open/save, workspace layout, selection, object transforms, primitive
creation, registered tool invocation, undo, and redo.

Workspace updates are presentation state, not project revisions. A layout must
contain a viewport and cannot contain duplicate panes; invalid updates become
diagnostics and leave the previous layout unchanged.

The session dispatches all authoring changes through project-bound editor
commands. A failed action records its diagnostic and does not create history.
New/open replacement and application close reject dirty state until the caller
provides an explicit discard decision. Opening a malformed or unavailable
project is performed into a temporary document, so the current project remains
active when loading fails. The snapshot method builds a temporary render
candidate and publishes it only if every visible instance compiles, resolves,
and passes the render boundary; a failed candidate yields an unavailable
viewport instead of a partial or invented fallback. Diagnostics are bounded to
the latest 128 entries. Project identity generations reset transient inspector
state after replacement. Compiled vertices and triangles retain stable source
IDs so a future viewport can map a hit back to editor selection without
exposing editable topology.

The optional desktop shell consumes only this action/snapshot interface. Its
outliner, inspector, and viewport are therefore presentation code; transform,
vertex, face, extrusion, creation, and history actions still pass through the
same session and command preconditions.

## Native backend boundary

`carto_vulkan` is an optional runtime probe and backend seam. It is not linked
by the default headless build. `CARTO_BUILD_DESKTOP=ON` additionally requires
Windows, a discoverable Vulkan SDK, and a caller-supplied pinned Dear ImGui
tree with the Win32 and Vulkan backends. There is no dependency download or
CPU rendering fallback. Missing prerequisites stop configuration or startup
with an explicit diagnostic; GPU execution and desktop workflow acceptance
remain separate verification gates.

Asynchronous jobs and persistent GPU resource ownership are not part of the
headless/application contract yet. When derived jobs or resources are
introduced, publication must verify the source revision before replacing a
derived result. That rule is already represented by `CompiledMesh::source_revision`
and `RenderScene::upsert`.
