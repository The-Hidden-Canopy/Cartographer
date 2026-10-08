# Cartographer project format v7

Cartographer v7 is the current deterministic `.carto` authoring format. It
retains the v6 authoring envelope, mesh attributes and UV sets, topology
receipt ledger, optional evaluation-graph digest, and provider-neutral
scientific model. Version 7 adds a required, independently versioned
`WORLD_MODEL` record containing five public authoring primitives:

- `SemanticArea` for stable semantic regions bound to an optional scene object
  or mesh asset;
- `SemanticPath` for bounded, finite, ordered spatial networks;
- `StructureGraph` for typed building, interior, and access relationships;
- `InstanceField` for deterministic recipe, seed, extent, population bound,
  and sparse suppression identity; and
- `VariantLayer` for sparse property overrides bound to a project revision.

New saves write v7. The parser continues to read v1 through v6; those projects
load with an empty world model and upgrade on their next save. A v7 document
must contain the record even when the model is empty. Unknown versions,
oversized counts, duplicate identities, unresolved cross-references, hierarchy
cycles, non-finite geometry, invalid authoring bindings, future variant base
revisions, and trailing data fail closed.

The terminal project records are:

```text
TOPOLOGY_RECEIPTS <count>
TOPOLOGY_RECEIPT <mesh-asset> "<receipt>"
EVALUATION_GRAPH_DIGEST <sha256>       # optional
SCIENTIFIC_MODEL "<versioned model>"
WORLD_MODEL "<versioned world model>"
END
```

The nested world model has its own magic and schema version and canonically
orders map identities and variant edits. Its digest is therefore independent
of insertion order where ordering has no authoring meaning. Path control-point
order remains semantic and is preserved.

World authoring state is mutated only through the existing project transaction
boundary. A commit validates all primitive references before the journal append
and publishes the staged document only after that append succeeds. A semantic
area binding prevents its scene object or mesh asset from being removed until
the binding is changed in another valid transaction.

This public record contains no runtime entity or device handle, executable
command, provider credential, adaptive representation policy, certification
algorithm, private telemetry, or VANTA admission state. Materialization and
runtime consumption remain separate derived-product boundaries.
