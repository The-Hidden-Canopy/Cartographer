# Cartographer project format v2 (legacy)

Cartographer v2 is the legacy deterministic line-oriented authoring model from
v1 with an explicit authoring envelope before the document name. New saves use
[project format v3](cartographer-project-v3.md), which adds the optional
content-addressed evaluation-graph reference:

```text
CARTOGRAPHER_PROJECT 2
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
END
```

The authoring envelope is intentionally small and explicit:

- `cartographer.authoring` identifies the owning document contract;
- `meters` is the unit contract for positions and translations;
- `right_handed_y_up` is the coordinate contract used by the native authoring
  and private consumer adapter paths.

All geometry and transforms remain double precision in the `.carto` source.
Derived formats may narrow or triangulate only at their own validated adapter
boundary and must report that loss. No compiled render data, GPU handles,
materials, textures, or AI/runtime state is stored in the authoring file.

The parser continues to accept v1 and v2 files. A legacy load is an explicit
legacy read; the next save writes v3. Unknown or future schema versions,
missing or mismatched v2 envelope fields, duplicate records,
trailing data, malformed topology, non-finite values, path traversal, and
resource-limit violations fail closed.

Private consumers may adapt this contract on their side. That does not make
Cartographer link against a consumer runtime or move runtime state into the
authoring document.
