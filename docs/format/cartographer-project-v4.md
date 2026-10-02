# Cartographer project format v4

Cartographer v4 keeps the v3 authoring envelope and adds a bounded, durable
lineage ledger for project-bound topology edits. New saves write v4. The
parser continues to read v1, v2, and v3 documents; a legacy document has an
empty receipt ledger until a project-bound topology edit is accepted.

```text
CARTOGRAPHER_PROJECT 4
AUTHORING "cartographer.authoring" "meters" "right_handed_y_up"
NAME "..."
REVISION <unsigned integer>
OBJECTS <count>
OBJECT <id> <parent-id-or-zero> "name" <TRS> <visible> <locked> <mesh-id-or-zero>
MESHES <count>
MESH <id> <mesh-revision>
VERTICES <count>
VERTEX <id> <x> <y> <z>
FACES <count>
FACE <id> <vertex-count> <vertex-id>...
TOPOLOGY_RECEIPTS <count>
TOPOLOGY_RECEIPT <mesh-asset-id> "<serialized topology receipt>"
EVALUATION_GRAPH_DIGEST <64 lowercase hexadecimal characters>
END
```

The graph record remains optional. The topology receipt record contains a
versioned `CARTOGRAPHER_TOPOLOGY_RECEIPT 1` value with mesh source/destination
revisions, created/removed stable IDs, and domain-local origin maps. It never
contains positions, compiled indices, GPU handles, prompts, credentials,
telemetry, or runtime entity IDs. The quoted receipt may span physical lines;
the parser consumes the quoted value as one bounded field with a 32 MiB byte
limit and a bounded aggregate identity/source-element budget.

Receipt records are historical lineage, not an alternate mesh source of
truth. A record must validate, reference an existing mesh asset, bind to a
non-exhausted advancing mesh revision interval, and be unique by mesh asset
and revision interval. Records newer than the current mesh snapshot, dangling
asset references, oversized counts, unknown origin kinds, duplicate IDs,
trailing data, and future receipt/project versions fail closed.

Only project-bound extrusion, inset, face deletion, and edge split commands
currently append receipts. Direct kernel calls and other project replacements
remain valid but do not claim a receipt unless they provide one through the
revision-bound replacement API. Undo and redo persist authoritative snapshots
and do not fabricate inverse topology receipts; existing lineage remains
historical and revision-bound.

The v4 ledger does not make the document, journal, package manifest, database,
and blob store one power-loss-atomic transaction. That cross-file boundary,
SQLite authoring storage, and receipt coverage for future modeling operators
remain separate acceptance gates.
