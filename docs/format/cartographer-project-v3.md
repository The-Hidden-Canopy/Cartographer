# Cartographer project format v3

Cartographer v3 keeps the deterministic authoring envelope from v2 and adds
an optional reference to an immutable, content-addressed evaluation-graph
snapshot:

```text
CARTOGRAPHER_PROJECT 3
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
EVALUATION_GRAPH_DIGEST <64 lowercase hexadecimal characters>
END
```

`EVALUATION_GRAPH_DIGEST` is omitted when the document has no graph binding.
It is not the graph payload; the package blob store owns the immutable graph
bytes and the package manifest owns the corresponding blob reference. A
`ProjectTransaction` changes the document reference through the normal
revision path, so journal envelopes and checkpoint snapshots preserve it.
`ProjectPackage::validate_document_binding` is the explicit cross-file check
that verifies the document and package reference agree and that the blob is
present and intact.

The v3 parser remains able to read v1 and v2 documents. New saves always write
v3. Zero, malformed, duplicate, or misplaced graph records fail closed, as do
unknown/future versions, invalid authoring envelopes, malformed topology,
trailing data, non-finite values, and resource-limit violations.

The graph reference does not make the package manifest, document database,
journal, and blob store one power-loss-atomic transaction. That larger storage
boundary remains explicitly deferred.
