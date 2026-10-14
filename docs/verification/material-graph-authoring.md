# Native material graph authoring

The Material instrument can create a four-output starter graph from an
existing project's PBR material factors, add constant, parameter, surface-input,
and arithmetic/mix nodes, edit constant and parameter values, change typed input
links, and route compatible nodes to material outputs. The source graph remains
in the Cartographer-owned material catalog.
Graph edits pass through a human-only application action and the existing
project material-catalog command, so journal, undo, redo, and `.carto`
save/reopen use the same durable boundary as other material edits.

An update must match the current project revision, source graph revision, and
canonical digest. The replacement graph must validate and advance its source
revision exactly once. A new graph starts at revision one. Stale requests,
missing graph identities, revision jumps, invalid graphs, and malformed
revision/digest pairs fail without publishing a project mutation.

Graph nodes receive monotonically increasing IDs within the current graph.
Removing a supported node is allowed only after it is disconnected from all
consumers and outputs; the removed ID is stored in a bounded, sorted tombstone
set. Updates must preserve every prior tombstone and record each removal
exactly once, so a deleted ID cannot be reused. Unsupported nodes are retained
until their payload format is understood.
Input links and output assignments are type-filtered, and each proposed
topology change is structurally validated before replacing the local draft.
Cycles, incompatible types, and broken required outputs are rejected without
mutating the draft. Texture-sample nodes and unsupported nodes already in a
graph are preserved, but texture source selection and edits to unsupported
payloads are not offered. Structural support diagnostics are not a shader
compiler result: no DXIL, GPU program, rendered material, or live viewport
shading is produced by this workflow.

Materials can be associated with a graph source through a separate human-only
command. The association pins graph identity, source revision, and canonical
digest in the material catalog. If the graph is edited later, the old
association remains intact and is projected as stale; only an explicit rebind
can adopt the new revision. Binding and unbinding advance the material source
revision and use journal, undo/redo, and `.carto` persistence. This records
authoring intent only; it does not evaluate the graph or affect rendered pixels.

Verification:

- `cartographer_material_graph_tests` checks typed graph validation,
  deterministic serialization, v1-to-v2 migration, tombstone validation and
  persistence, stale-source checks, and retained unsupported nodes.
- `cartographer_application_tests` checks graph creation/edit, exact revision
  advancement, stale project/digest rejection, invalid revision rejection,
  typed topology/output changes, cycle rejection without project mutation,
  refusal of untracked removals and identity reuse, pinned graph bindings,
  stale projection after graph edits, explicit rebind/unbind, unsupported-node
  preservation, journal receipts, undo/redo, and save/reopen.
- `cartographer_ui_tests` checks native snapshot projection, graph-source read,
  human graph factor/topology edits, revision-pinned binding, operation
  receipts, and rejection of raw AI mutation.

Remaining work includes texture selection and asset-backed texture intake,
backend shader lowering, live viewport evaluation, and CONDITION
production-board acceptance.
