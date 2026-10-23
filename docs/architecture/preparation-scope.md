# Preparation and export scope

Preparation scope is captured by stable IDs at submission time. The resolver
never rereads a later UI selection and rejects a request whose expected project
revision is stale.

The public scope kinds are:

- `asset`: one explicit reusable mesh asset;
- `selection`: explicit node and asset IDs plus required node ancestors and
  referenced assets;
- `assembly`: one root node plus descendants, ancestors, and referenced assets;
- `scene`: every authored node and mesh asset.

Resolved vectors are sorted, unique, non-zero, revision-bound, and validated
against the current `ProjectDocument`. A prepared-scene composition must receive
exactly the products named by its resolved scope; extra or missing products fail
instead of silently changing the request.

OBJ, PLY, and STL remain single-mesh formats in Cartographer. Their CLI accepts
an explicit mesh asset ID:

```text
cartographer_cli export-obj project.carto 42 mesh.obj
cartographer_cli export-ply project.carto 42 mesh.ply
cartographer_cli export-stl project.carto 42 mesh.stl
```

For compatibility, the ID may be omitted only when a project contains exactly
one mesh. Multi-mesh projects fail with an explicit-scope diagnostic; the first
map entry is never chosen implicitly. The desktop resolves the current stable
object selection and permits a single-mesh export only when that selection
closes to exactly one reusable asset.

glTF remains the documented whole-project scene export. It does not reuse the
single-mesh fallback.
