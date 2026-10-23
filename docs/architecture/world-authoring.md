# Provider-neutral world authoring

`carto_world` is Cartographer's public authoring substrate for spatial meaning
that should survive changes in renderer, runtime, and generated representation.
It deliberately owns six primitives and no execution policy:

| Primitive | Authoritative public content | Deferred/private consumer behavior |
| --- | --- | --- |
| `SemanticArea` | stable ID, semantic type, optional parent, sorted tags, optional scene-object or mesh binding | runtime region creation, streaming, representation choice |
| `SemanticPath` | stable ID, ordered finite control points, width, optional terminal areas | road/river/nav meshing, simulation, path scheduling |
| `WorldRegion` | stable ID, exact revision, signed authored cell coordinate, bounded area/path memberships, reciprocal neighbors, explicit cross-region path references | geographic bounds, runtime streaming/residency, automatic partitioning |
| `StructureGraph` | stable typed nodes and edges, optional area/path references | building/interior materialization and gameplay access logic |
| `InstanceField` | stable ID, finite bounds, recipe identity/digest, seed, maximum population, sorted sparse suppressions | proprietary population algorithms, runtime spawning, residency |
| `VariantLayer` | stable ID, project base revision, bounded canonical property edits | game-state selection, adaptive policy, runtime promotion |

The world model is not a second scene and does not hold a mutable project
pointer. A detached value can be assembled and validated, but it enters a
project only through `ProjectTransaction::set_world_model`. The staged project
is validated before its journal append, and only a successful append publishes
the staged state. Scene-object and mesh bindings must resolve in that same
staged document. Bound authoring objects cannot be deleted while referenced.

Cross-reference validation runs over the complete model. Area parents must
exist and remain acyclic; path terminals, graph area/path references, instance
field areas, region memberships, region neighbors, cross-region paths, and
variant targets must resolve. Region coordinates are authored integer keys,
not inferred geographic bounds. Neighbor links are reciprocal. A path joining
areas assigned to different regions must be explicitly listed as a
cross-region path by both neighboring regions. Counts, strings, control
points, bounds, population sizes, memberships, edit batches, and sparse edits
are bounded. Region edits carry exact per-region revisions and publish
atomically only after the whole candidate model validates. Non-finite
geometry, duplicate IDs, duplicate target-property edits, hostile counts,
unsigned wraparound, unknown framing, and trailing bytes fail closed.

The nested serializer canonically orders identities and variant edits where
order has no semantic meaning. Path control-point order is preserved. The
canonical SHA-256 digest can therefore bind a derived production source without
embedding local paths, device handles, telemetry, or a selection algorithm.

## Runtime boundary

This layer does not choose a representation, generate geometry, certify a
candidate, materialize entities, schedule workers, allocate GPU resources, or
admit downstream consumer state. Those are derived-product/runtime responsibilities. A
consumer may use the public stable IDs and digest, but must publish its own
source-bound artifact and decision receipts and revalidate them against the
current project revision before consequence.
