# Cartographer project schema v35

Schema v35 adds a revisioned table of canonical authored kinematic graphs.
`KINEMATIC_GRAPHS` follows `CONNECTION_PORT_GRAPH` and precedes the outer
`END` record. Each `KINEMATIC_GRAPH` row stores a stable project-local
identity, record revision, SHA-256 digest, and quoted canonical
`KinematicGraph` serialization.

The project validates each graph through the public kinematic codec and
requires its digest to match the exact bytes. A graph insert uses expected
revision zero; replacement requires the current record revision. Both paths
run through `ProjectTransaction`, so the graph joins the journaled project
snapshot and the existing undo/redo and save/reopen path. The table is bounded
to 4,096 records, 16 MiB per graph, and 64 MiB total serialized graph bytes.

Schemas 1 through 34 remain readable. Older projects receive an empty graph
table when next serialized. Schema-35 projects must include the table even
when empty, and graph rows are refused when labeled as an older schema.
Kinematic graphs carry hierarchy, rest transforms, joint kind, deformation
class, and authored semantic type; they do not include controls, skin weights,
mesh binding, or preview-local autorig candidates.

The project transaction API makes a caller-supplied final graph durable. It
does not convert `AutorigCandidate` IDs or infer missing rig fields. A human
review acceptance is still not a committed rig until a separate mapping step
provides explicit stable joint IDs and authored final-node fields.
