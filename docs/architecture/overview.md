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
| `carto_assets` | content-addressed immutable blob bytes and digest verification | authoring ownership or import policy |
| `carto_project` | versioned authoring persistence and asset references | render caches |
| `carto_journal` | bounded revision-bound recovery records and hash-chain verification | explicit save state or editor history ownership |
| `carto_providers` | capability descriptors and explicit provider lifecycle | dynamic code loading or permissions |
| `carto_plugin_protocol` | bounded extension manifests and framed proposals | process isolation, project mutation, or network authority |
| `carto_plugin_package` | bounded `.cartoplug` inspection, signature verification, local install receipts | plugin execution, sandboxing, Hub authority, or project mutation |
| `carto_editor` | commands and undo/redo history | direct panel-owned mutations |
| `carto_render` | compiled render snapshots | editable topology |
| `carto_render_graph` | backend-neutral resource/pass validation and compiled hazards | GPU submission or authoring truth |
| `carto_gpu` | typed session-local resource handles and public descriptors | Vulkan handles, device ownership, or project serialization |
| `carto_sdk` | bounded opaque C ABI over application actions/snapshots | C++ layout, mutable model pointers, or native GPU handles |
| `carto_io` | format adapters and loss reports | project model ownership |
| `carto_application` | front-end actions, workspace state, immutable snapshots | direct panel-owned mutations or GPU resources |
| `carto_ui` | authority-preserving presentation state, design tokens, bounded UI preferences, operation/problem projections | project truth, editor history ownership, direct mutation, AI authority |
| optional native shell | Win32 windowing, ImGui panels, Vulkan submission | authoritative model state or editable topology |

## Failure posture

Rejected inputs return a `Diagnostic` with a stable error category. The 0.1
implementation rejects malformed topology, hierarchy cycles, dangling
references, stale render revisions, unsupported project schema versions,
invalid project-relative asset paths, and failed atomic writes. It does not
silently repair or flatten these cases.

Primitive constructors are deterministic input tools rather than a second
source of truth: they generate ordinary editable mesh authoring data. Mesh
authoring now retains stable edge, half-edge, and corner identity records
across topology reads; face-boundary traversal is deterministic and preserves
those identities when an adjacent face is added. Vertex position edits use a
revision-bound atomic `MeshPatch` and validate the whole mesh before
committing a revision; a failed patch restores the prior position and cannot
enter command history.

Single-face extrusion removes the source face, creates the side walls and cap,
then validates the complete mesh. Any failed generated vertex or face restores
the pre-operation mesh before returning a diagnostic; its editor command keeps
validated before/after topology for reversible history.

Project-bound editor commands also carry the mesh revision they last applied.
Undo and redo use a revision-conditional replacement, so an external project
edit causes a stale-data failure instead of being silently overwritten.

Compiled CPU geometry is cached by the authoring mesh revision and invalidated
whenever an authoring mutation advances that revision. The cache is derived
state only: it is not serialized, does not own topology, and does not imply a
GPU resource cache or asynchronous evaluation system.

The public render foundation adds typed generational GPU handles, deferred
destruction retirement, a canonical offscreen viewport description, and a
headless render-graph compiler. It validates resource descriptors,
read-before-write hazards, write transitions, stage/format compatibility, and
alias lifetime overlap without executing a backend. GPU handles are ephemeral
and cannot enter project serialization.

The storage foundation adds a content-addressed SHA-256 blob store and a
revision-bound append-only journal with tamper/truncation detection. Its
checkpoint store persists snapshot bytes by digest and records the verified
journal prefix needed to identify pending entries after restart. These are
standalone primitives: the v1 text project serializer remains authoritative
until a real package/database integration is accepted, and journal presence is
not equivalent to an explicit user save.

Provider resolution is metadata-only at this stage. `carto_providers` keeps
discovery, inspection, registration, verification, readiness, loading, and
distinct failure states explicit. `carto_plugin_protocol` validates bounded
sandbox-default manifests and `carto.plugin.v1` frames; it does not launch a
process or admit plugin output into the project.

Selection state is an ephemeral editor concern. Object, vertex, edge, and face
selections are isolated by mode, are never serialized into project truth, and
must be validated against the current scene or mesh before a tool consumes
them. Component selections may be bound to one scene object; add/toggle
operations cannot span objects or silently bind an existing unbound selection
to a different mesh context. Mesh-bound selections retain the mesh instance and
source revision they were created against, so replacement or mutation requires
explicit reselection. Edge selection consumes persistent topology identity.
Single-edge splitting, convex-face inset, and single-face deletion are now
admitted topology edits: they validate the bound selection, mutate a
project-owned mesh through a revision-guarded command, emit
affected-set/parameter receipt metadata, and clear selections whose identities
are removed. Concave inset, broader edge traversal, and multi-selection
topology operators remain deferred.

Tool actions are registered by stable IDs and return editor commands. The
registry submits successful commands through `CommandBus`; it does not expose a
direct mutation callback or mutable mesh accessor to panels or UI code. The
current built-in context resolves an object-bound selection through
`ProjectDocument` to the owning mesh asset and exposes approved command
builders for selected-face extrusion, convex-face inset, single-face deletion,
single-edge splitting, and single-vertex position editing.
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
commands and requires a named front-end admission object; an unadmitted caller
cannot invoke the public dispatch route. A failed action records its diagnostic
and does not create history.
New/open replacement and application close reject dirty state until the caller
provides an explicit discard decision. Opening a malformed or unavailable
project is performed into a temporary document, so the current project remains
active when loading fails. The snapshot method builds a temporary render
candidate and publishes it only if every visible instance compiles, resolves,
and passes the render boundary; a failed candidate yields an unavailable
viewport instead of a partial or invented fallback. Diagnostics are bounded to
the latest 128 entries. Project identity generations reset transient inspector
state after replacement. Compiled vertices and triangles retain stable source
IDs so a viewport can map object, vertex, edge, and face hits back to editor
selection without exposing editable topology.

The optional desktop shell consumes only this action/snapshot interface. Its
outliner, inspector, and viewport are therefore presentation code; transform,
vertex, edge, face, extrusion, creation, and history actions still pass through
the same session and command preconditions.

`carto_ui` adds the testable presentation contract above the application
session. Its operation rail is a bounded view of successful application
receipts, not a second history or audit ledger. Theme/density/workspace state
and local preferences remain outside project truth; corrupt preferences reset
through the same validated workspace action path. The controller exposes the
AI-First shell only as an unavailable/manual state until a bounded planner and
proposal transaction adapter exist.

## Native backend boundary

`carto_vulkan` is an optional runtime probe and backend seam. It is not linked
by the default headless build. `CARTO_BUILD_DESKTOP=ON` additionally requires
Windows, a discoverable Vulkan SDK, and a caller-supplied pinned Dear ImGui
tree with the Win32 and Vulkan backends. There is no dependency download or
CPU rendering fallback. Missing prerequisites stop configuration or startup
with an explicit diagnostic; GPU execution and desktop workflow acceptance
remain separate verification gates.

The headless build also exposes a backend-neutral `carto_gpu` contract for
typed generational resource handles, deferred destruction keyed by completed
submission serials, and canonical resource descriptions. `carto_render_graph`
validates declared resource hazards, produces write barriers, and rejects
overlapping alias lifetimes. `render::OffscreenViewport` declares canonical
color/depth/selection/normal attachments and increments a generation when its
extent or attachment policy changes. These are planning and lifetime
contracts only; they do not claim Vulkan allocation, command submission, or
GPU execution.

`carto_assets` provides the first persistent-data boundary: immutable bytes are
stored beneath a SHA-256 content path, written through a temporary file and
atomic rename, and re-hashed on reads. `carto_journal` provides the first
recovery boundary: committed records carry before/after revisions, payload
digests, a previous-entry hash, and a record hash. The application session
binds that journal to saved projects, records a baseline plus accepted
mutations, and rolls back an in-memory action if its durable append fails.
These foundations are not yet a replacement for the v1 text project file:
the SQLite WAL package, checkpoint replay, and startup recovery flow remain
separate work.

Asynchronous jobs and persistent GPU resource ownership are not part of the
headless/application contract yet. When derived jobs or resources are
introduced, publication must verify the source revision before replacing a
derived result. That rule is already represented by `CompiledMesh::source_revision`
and `RenderScene::upsert`.
