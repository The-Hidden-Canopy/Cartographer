# Cartographer project format v2

Cartographer v2 keeps the deterministic line-oriented authoring model from v1
but adds an explicit authoring envelope before the document name:

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
  and private VANTA adapter paths.

All geometry and transforms remain double precision in the `.carto` source.
Derived formats may narrow or triangulate only at their own validated adapter
boundary and must report that loss. No compiled render data, GPU handles,
materials, textures, or AI/runtime state is stored in the authoring file.

The parser continues to accept v1 files. A v1 load is an explicit legacy read;
the next save writes v2 with the authoring envelope. Unknown or future schema
versions, missing or mismatched v2 envelope fields, duplicate records,
trailing data, malformed topology, non-finite values, path traversal, and
resource-limit violations fail closed.

The private VANTA adapter consumes this contract on the VANTA side. It does
not make Cartographer link against VANTA or move VANTA runtime state into the
authoring document.
