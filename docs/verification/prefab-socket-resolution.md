# Prefab socket resolution

## Implemented slice

`condition::resolve_prefab_socket` joins a declared prefab socket name to one
exact source-member `ConnectionPort` and resolves its named parent-frame chain.
The returned transform is expressed in the prefab root's local coordinates.
It carries every port ID and canonical digest in the socket-to-root frame chain
so a later consumer can bind its review to all source records that shaped the
composed transform.

The resolver refuses undeclared or duplicate socket matches, foreign authoring
bindings, stale prefab revisions, missing or ambiguous parent frames, cycles,
over-deep frame chains, and legacy instance records without isolated snapshot
bindings. It trusts those records as inputs; it does not verify that the
referenced meshes exist or match their receipts. It remains a read-only
primitive. The application now performs the live source/project checks, stages
root-owned instance port snapshots, and asks the persistent project graph to
admit a connection. It never snaps or moves scene objects.

## Acceptance state

`tests/prefab_socket_tests.cpp` covers parent-frame composition, undeclared
names, stale prefab revisions, duplicate matches, foreign source bindings,
cyclic/missing/over-deep parent frames, and unisolated legacy instances.
`tests/condition_prefab_placement_tests.cpp` now authors a source port graph,
connects two current prefab instances, checks persisted compatibility evidence,
round-trips the project graph, and checks stale-graph refusal, same-instance
refusal, explicit edge/port removal, and undo/redo. These tests were authored
but not built or run. `tests/ui_tests.cpp` also authors public-controller
coverage for adding and removing a connection port while no CONDITION source
document is loaded, including history and undo/redo; this was not built or run.
No native interaction was performed.

## Remaining work

Project schema 34 persists the `ConnectionPortGraph`. Application and UI
controller actions bind graph edits to the expected project state and connect
current prefab instances by exact declared socket name; the graph applies tag,
capability, namespace, duplicate-edge, and cycle checks before one journaled
replacement. Existing instance socket snapshots are reused only while they
match the latest source resolution. A stale snapshot requires an explicit graph
edit; object deletion is refused while its ports remain registered. The native
maintenance panel exposes explicit edge disconnect and port removal actions.
Connected ports cannot be removed until every edge is disconnected, and both
steps use the project journal.

The desktop CONDITION panel now creates source-member port frames with explicit
frame convention, local position/rotation, compatibility kind, and term. It
lists sockets from current prefab instances and dispatches selected endpoint
connections across two distinct instances through the project journal.
Source-port identity is assigned by the application under the graph digest and
project revision. Graph cleanup is exposed in the panel even when no CONDITION
authoring document is loaded, since the typed graph is project data. Its port
and edge lists clip visible rows rather than allocating a label for every
record. Selection is retained by port/edge identity, and removal is disabled
until the user explicitly selects a live record; list changes cannot retarget
the selection by index. Mutation actions still bind the current project
generation, revision, and canonical graph digest. This UI continuation has not
been built or exercised in the native app. Downstream/runtime
attachment behavior remains open; no snap transform or runtime attachment
state is claimed.
