# Face-side selection

In Modify, switch to face mode and select at least one face on each mesh to inspect. **Triangles**, **Quads**, and **N-gons** replace each owner's face selection with all authored faces having respectively three, four, or more than four sides. This inspects authored polygon boundaries, not triangles produced by viewport compilation. A mesh without matches contributes an empty selection.

These commands change only ephemeral selection, not geometry or the saved project. They retain the existing project-snapshot and mesh-revision checks and process selected owners independently. They do not repair or triangulate a mesh.

Implementation status: source wiring is present, but this increment has not been built or run and native click-through acceptance has not been performed.
