# Evaluation graph and attribute domains

The gap-closure series identifies evaluation as the force multiplier for
modifiers, construction history, procedural geometry, animation, materials,
and CAD features. Cartographer now has the first backend-neutral structural
rung in `carto_eval` and `carto_attributes`.

## Evaluation graph

`EvaluationGraph` owns only graph intent and revisioned node metadata. It does
not own project authority, mutable meshes, GPU handles, or worker threads.
Each node has a stable `NodeId`, registered `NodeTypeId`, typed ports,
parameter revision, and a caller-provided parameter digest. Connections are
validated before publication and the graph rejects missing ports, incompatible
types, multiple writers to one input, self-cycles, and general cycles.

The graph produces a deterministic topological order and a SHA-256 content
digest. `EvaluationCache` keys outputs by node, parameter revision, and
dependency digest. `EvaluationTicket` plus `validate_publication` is the stale
worker boundary: a result may publish only when graph revision, node parameter
revision, and dependency digest still match.

The graph also has a bounded, versioned text snapshot format for save/reopen
of graph intent. Deserialization is size-limited, rejects unknown versions,
duplicate identifiers, unsafe token fields, invalid ports, missing nodes, bad
edges, cycles, and trailing data. This is graph persistence only; it is not a
project package, journal checkpoint, or evaluator result store.

`EvaluationScheduler` provides the worker boundary with a bounded outstanding
job count, ticket validation at submission, exception-to-receipt conversion,
graceful draining, and candidate polling. It deliberately has no method that
mutates `EvaluationGraph` or `EvaluationCache`; callers must recheck the ticket
and explicitly publish a validated candidate.

`ProjectDocument` may carry an optional content-addressed graph digest. A
`ProjectTransaction` changes that reference through the same revision path as
scene and mesh edits; its serialized document is the journal payload and the
checkpoint snapshot, so recovery cannot silently lose the graph binding. The
package layer stores and verifies the immutable graph bytes separately. A
caller still has to bind the digest returned by the package store, and the
multi-file package/document commit remains open.

This is not yet a full evaluator. It intentionally does not invent mesh,
curve, material, or B-Rep result payloads. Those typed result domains can be
added behind the same graph contract once their owning kernels exist.

## Attribute domains

`carto_attributes` keeps attribute cardinality per domain rather than using a
single mesh-wide count. Initial domains include object, vertex, edge, face,
corner, curve point, spline, joint, control point, B-Rep face, and B-Rep edge.
Initial types include scalar numerics, vectors, colors, and stable ID values.

UVs therefore belong in a corner-domain layer, while material slots can belong
to a face-domain layer without pretending those two layers have the same
cardinality. `TopologyProvenance` maps source indices separately for each
domain. `AttributeTransferPolicy` makes preserve, interpolate, duplicate, and
invalidate behavior explicit; discrete values reject interpolation instead of
silently averaging IDs. A topology mapping may contain an empty source set for
a newly-created destination element; only explicit invalidation may consume
that mapping, while preserve/interpolate/duplicate fail closed.

`StableTopologySnapshot` and `derive_provenance` provide a backend-neutral
adapter for kernels that expose stable element IDs. Unchanged IDs map to source
indices, new IDs become empty mappings, and duplicate or zero IDs are rejected.
`carto_mesh_attributes` now supplies the polygon-kernel adapter for stable
vertex, edge, face, and corner IDs plus `provenance_from_receipt`, which binds a
validated kernel receipt to the source and destination revisions and replaces
created-element empty mappings with explicit domain-local source indices.
Unknown identities and stale source/destination revisions fail closed. The
adapter does not infer provenance for a kernel that has not supplied stable
identity; curve and B-Rep owners still need to wire their operation receipts
into this contract.

## Boundary still open

The next structural rung is atomic package/document graph storage and broader
topology-operation coverage. Extrude, split-edge, and inset now emit explicit
lineage for the polygon kernel; delete emits a validated identity delta but has
no created-element lineage by design. Bevel, merge, dissolve, bridge, loop cut,
knife, modifiers, sculpt, animation, and CAD claims remain deferred until their
operators have receipts, attribute-survival tests, and workflow evidence.
