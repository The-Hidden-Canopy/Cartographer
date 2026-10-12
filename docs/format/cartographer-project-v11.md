# Cartographer project format v11

Cartographer v11 retains every v10 record and adds a per-mesh `SCULPT_STACKS`
record after the optional semantic sidecar:

```text
CARTOGRAPHER_PROJECT 11
...
MATERIAL_CATALOG "<versioned source catalog>"
SEMANTIC_SIDECAR "<sha256-hex>" "<opaque serialized sidecar bytes>"  # optional
SCULPT_STACKS <count>
SCULPT_STACK <mesh-asset-id> <sha256-hex> "<serialized SculptStack bytes>"
...
END
```

The `SCULPT_STACKS` record is required in v11, including when the count is
zero. Each stack is associated with one existing mesh asset and carries a
SHA-256 digest of its exact serialized bytes. ProjectDocument preserves the
stack bytes without depending on the sculpt compiler; the production
SculptStack deserializer validates the layer/stroke records and the compiler
rebinds their source namespace, mesh identity, revision, and content digest
before producing evaluated geometry. Authoring stack changes go through
`ProjectTransaction` and the project journal. Evaluated vertex positions,
preview buffers, and renderer state are not stored as SculptStack source.

The project reader rejects duplicate or unknown mesh bindings, zero or
mismatched digests, empty or oversized stacks, and v10-or-older files that
claim `SCULPT_STACKS`. The project limits the table to 100,000 mesh stacks and
each stack payload to 16 MiB, within the existing 128 MiB project limit.
SculptStack source payloads have their own stricter canonical format and
source-binding checks.

Versions 1 through 10 remain readable. Older files load without SculptStack
state and are saved as v11 with an empty stack table until a stack is
explicitly authored.
