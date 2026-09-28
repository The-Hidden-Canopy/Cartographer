# Cartographer project format v1

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

All authoring numbers are parsed as finite double precision values. IDs are
serialized explicitly and restored rather than regenerated. Unknown schema
versions fail closed. Render meshes, GPU resources, thumbnails, BVHs, and
other derived caches are intentionally absent.

## Save contract

`ProjectDocument::save_atomic` validates the in-memory document, serializes it,
parses the serialized bytes again, writes a sibling temporary file, flushes it,
and atomically replaces the target. A failure before replacement leaves the
previous target untouched. Recovery journals and large binary chunks are
planned, not claimed.

## Security boundary

External asset references are project-relative. Absolute paths, rooted paths,
and `..` traversal are rejected. The current format stores no executable
content or plugin code.

