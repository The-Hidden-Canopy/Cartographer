# Cartographer project format v1

This is the legacy format. Cartographer continues to read it for migration,
but new saves use [project format v2](cartographer-project-v2.md), which adds
an explicit authoring/units/coordinate-system envelope.

The 0.1 project format is a deterministic, line-oriented text format intended
to make failures diagnosable before a binary chunk store exists. It is not
JSON, glTF, or a compatibility promise with another application.

## Structure

```text
CARTOGRAPHER_PROJECT 1
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
END
```

All authoring numbers are parsed as finite double precision values, and object
rotations must also be non-degenerate and normalizable. IDs are serialized
explicitly and restored rather than regenerated. Unknown schema versions fail
closed. Duplicate asset records and trailing records after `END` are rejected
rather than ignored. The parser bounds each object, mesh,
vertex, and face collection, and each face's vertex list, at 1,000,000
records. It also rejects serialized input larger than 128 MiB. These are v1
resource-safety limits, not a claim that all such scenes are practical for the
editor. Hierarchy evaluation is bounded to 4,096 parent links so deeply nested
input fails closed instead of consuming unbounded call-stack or evaluation
time. Exhausted authoring revisions are rejected and revision advancement
saturates as a final wraparound defense. Parent links may refer to an object
record that appears later in the file; they are resolved only after all object
records are present. Render meshes, GPU resources, thumbnails, BVHs, and other
derived caches are
intentionally absent.

Runtime `EdgeId`, `HalfEdgeId`, and `CornerId` records are maintained by the
authoring mesh and are intentionally not additional v1 serialized fields yet.
They preserve identity across topology reads and in-session edits; durable
topology-ID persistence requires a future schema migration rather than being
silently implied by the current text format.

## Save contract

`ProjectDocument::save_atomic` validates the in-memory document, serializes it,
parses the serialized bytes again, writes a sibling temporary file, flushes and
closes it successfully, and atomically replaces the target. A failure before
replacement leaves the previous target untouched. Recovery journals and large binary chunks are
planned, not claimed.

## Security boundary

External asset references are project-relative. Absolute paths, rooted paths,
and `..` traversal are rejected. The current format stores no executable
content or plugin code.
