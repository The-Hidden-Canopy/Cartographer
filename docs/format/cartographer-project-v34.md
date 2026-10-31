# Cartographer project schema v34

Schema v34 adds the typed connection-port graph to the durable project
document. The graph is stored as quoted canonical `ConnectionPortGraph`
bytes immediately before `END`; it retains authored ports, accepted edges,
and the compatibility evidence captured when each edge was created.

The graph has its own versioned codec and bounds. Project loading validates
the graph and requires every port owner to resolve to a scene object. Deleting
an object with registered ports fails until a human graph edit explicitly
removes the affected edges and ports. Graph replacement is project-revision
bound and passes through the existing project command journal.

Projects from schemas 1 through 33 remain readable with an empty graph.
Schema-34 files must include the graph record, including when it is empty.
Nonempty graph data is refused when labeled as an older project schema. The
outer project schema changes; the connection graph retains its independent
codec version.

Graphs whose next edge identity cannot be reconstructed from surviving edges
use nested graph version 2 with `NEXT_EDGE_ID` before `END`. This preserves
retired identities after deletion and reopen, including exhausted allocation.
Graphs with reconstructable allocation keep the version-1 layout. Readers
support both; older graph readers must refuse retained-history records rather
than dropping that history. The outer schema remains 34.

## Prefab socket connection path

The application resolves each declared prefab socket against the exact source
member ports, parent-frame chain, and current isolated instance. It creates a
root-owned connection-port snapshot using the resolved instance frame, then
asks `ConnectionPortGraph` to admit the requested edge. Compatibility tags,
capability requirements, edge-kind namespace, duplicate checks, and cycle
checks run before the candidate graph is staged. Source ports and instance
snapshots are saved together in one undoable project command.

The desktop source-port authoring action leaves identity assignment to the
application. It binds the expected project generation/revision and graph
digest, validates the owner against the scene, allocates the next stable port
identity, and writes the graph with the existing project command journal.
The maintenance panel removes one edge explicitly, then permits removal of an
unconnected port. Each action is human-only, graph-digest bound, and journaled;
port removal does not implicitly delete its edges.

Rebase does not rewrite an existing connection port snapshot. If its source
frame or compatibility contract changes, a subsequent connection request
detects the stale snapshot and requires an explicit graph edit before reuse.
Native connection authoring and display-backed acceptance remain separate
gates.
