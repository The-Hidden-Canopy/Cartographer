# Import and export guide

> Historical handoff from 2026-10-28, recovered during worktree consolidation. Workstation paths use placeholders. UI labels, source line references and unimplemented proposals describe that dated candidate. They are not verification of the current application. See [the current texturing verification](../verification/texturing-workflow.md) and [integration ledger](../verification/worktree-integration-20261116.md).


This guide covers the mesh interchange routes in the frozen Cartographer alpha
baseline. It was written against source at commit `fae4f7d` and every behavior
below is code-checked: menu labels, shortcuts, error text, and warnings are
quoted from the actual sources (`apps/desktop/main.cpp`, `apps/cli/main.cpp`,
and `libs/io`). Where a behavior has not yet been accepted on candidate native
builds, this guide says what the code does, not what a future build might do.

The desktop Vulkan workbench is `apps/desktop`; the D3D12 shell is a separate
app and is not covered here.

## At a glance

| Route | Import | Export |
| --- | --- | --- |
| Native desktop (File menu) | OBJ, PLY, STL via **File > Import Mesh…** (`Ctrl+I`) | glTF, GLB, OBJ, PLY, STL via File menu |
| CLI (`cartographer_cli`) | `import-obj`, `import-ply`, `import-stl` | `export-obj`, `export-ply`, `export-stl`, `export-gltf`, `export-glb` |
| glTF / GLB import | **No user import route** in native or CLI at this baseline | Yes (whole project) |

glTF/GLB is **export-only** as a user-facing route at this commit: there is no
native File-menu item and no `import-gltf` CLI subcommand. An application-layer
glTF scene import implementation does exist in this tree
(`libs/application/src/gltf_scene_import.cpp`, `ApplicationSession::import_gltf_scene`),
but it is not wired to any general desktop menu route, and the CLI exposes no
command for it; the only desktop caller is a governed internal promotion path
(botanical-runtime output, `apps/desktop/main.cpp` around line 17436), not a
user import path. So glTF import is **not a supported way to get content into
Cartographer** in this baseline. If you need to import glTF content today, you
must convert it to OBJ, PLY, or STL outside the app first and use
**File > Import Mesh…** or `import-obj/ply/stl`.

## Native desktop import

- **File > Import Mesh…** (shortcut `Ctrl+I`), or the **Import OBJ / PLY / STL**
  button on the Assets workbench, or the `file.import-mesh` command in the
  Ctrl+K command palette.
- A file dialog limited to `*.obj;*.ply;*.stl` asks for the source file. The
  format is chosen from the file extension; anything else shows
  "Choose an OBJ, PLY, or STL mesh file."
- The mesh is added as a new object in the **current open project**; nothing is
  written to disk until you save. The status line reports the object name,
  vertex/triangle counts, and the format used.
- Interchange warnings (for example, ignored OBJ records) are shown in a
  "Cartographer mesh import warnings" dialog. Warnings mean the import
  succeeded with dropped data; a hard failure shows "Cartographer mesh import
  failed" and changes nothing.

## Native desktop export

Exact File-menu labels and shortcuts (from `apps/desktop/main.cpp`):

| Menu item | Shortcut | What it exports |
| --- | --- | --- |
| **Export glTF** | `Ctrl+Alt+E` | Whole project scene to `.gltf` |
| **Export GLB** | *(no shortcut)* | Whole project scene to `.glb` |
| **Export OBJ** | `Ctrl+Alt+O` | One mesh asset to `.obj` |
| **Export PLY** | `Ctrl+Alt+P` | One mesh asset to `.ply` |
| **Export STL** | `Ctrl+Alt+S` | One mesh asset to `.stl` |

### The clean-save gate

Every export item is disabled unless the project is **saved and unmodified**.
The code checks `project_path` is set and the document is not dirty; when the
check fails the menu shows the disabled hint:

> Export requires a saved, unmodified .carto source

If triggered without a saved path you get "Save the .carto project before
exporting a … asset."; if the document has unsaved changes you get "Save the
project before exporting so the derived asset has a durable source revision."
Save with **File > Save** (`Ctrl+S`) or **File > Save As…** first. There is no
autosave and no previous-version backup in this baseline; if you discarded
work, **File > Recover latest journal snapshot** is the explicit recovery
route. If the export items stay greyed out, see
[Troubleshooting](troubleshooting.md#6-export-is-disabled-or-says-save-the-carto-project-before-exporting).

### Which mesh gets exported

- **glTF / GLB** exports the whole project (all objects and mesh assets) in one
  scene.
- **OBJ / PLY / STL** export exactly one mesh asset. The export resolves the
  mesh this way: if objects are selected, the selection must resolve to exactly
  one reusable mesh asset (otherwise "Select objects that resolve to exactly
  one reusable mesh asset."); if nothing is selected, the project must contain
  exactly one mesh asset (otherwise "mesh export is ambiguous; provide an
  explicit stable mesh asset ID"). A project like First Block, with one object,
  always resolves cleanly with nothing selected.

### Where exports go

Exports always prompt with a **save file dialog**; you choose the directory and
file name every time. Cartographer keeps **no default exports directory** for a
flat (non-prepared) project — nothing is written silently next to the `.carto`.
Export writes through a temporary file and atomically replaces an existing file
at the chosen path, so confirming an existing name overwrites it.

## CLI reference

Run `cartographer_cli --help` for the authoritative list. The interchange
subcommands at this baseline:

```text
cartographer_cli import-obj <mesh.obj> <project.carto>
cartographer_cli import-ply <mesh.ply> <project.carto>
cartographer_cli import-stl <mesh.stl> <project.carto>
cartographer_cli export-obj <project.carto> [mesh-asset-id] <mesh.obj>
cartographer_cli export-ply <project.carto> [mesh-asset-id] <mesh.ply>
cartographer_cli export-stl <project.carto> [mesh-asset-id] <mesh.stl>
cartographer_cli export-gltf <project.carto> <scene.gltf>
cartographer_cli export-glb <project.carto> <scene.glb>
```

### CLI imports create a new project — and can overwrite

`import-obj/ply/stl` always create a **brand-new project** at the target path
(named after the source file stem) and journal the import; they do not merge
into an existing project. The save is an atomic replace, so **if a file already
exists at `<project.carto>` it is overwritten without a prompt** — `import-*`
never asks first. `cartographer_cli demo <project.carto>` behaves the same way
(`apps/cli/main.cpp:268`): it creates a sample project and saves it at the
target path with no application-level overwrite check, so an existing file at
that path is replaced. The only creation command with an explicit refusal is
`cartographer_cli scientific-demo <project.carto>`, which fails with
"scientific demo refuses to overwrite an existing project or journal" when the
target project or its `.journal` already exists (`apps/cli/main.cpp:650`).
Always point `import-*` and `demo` at a new or empty path, and never aim them
at a project you have edited. (The First Block regeneration script likewise
refuses to overwrite and asks for an empty output directory.)

### Single-mesh vs multi-mesh export

`export-obj/ply/stl` take an **optional mesh asset ID** between the project and
the output path:

- Omit it only when the project contains **exactly one mesh asset** — that
  asset is exported.
- In a multi-mesh project, omitting it fails with "mesh export is ambiguous;
  provide an explicit stable mesh asset ID".
- Provide it as a **plain non-zero unsigned integer** (the stable mesh asset
  ID, e.g. `1`). Any other text fails with "export mesh asset id must be a
  non-zero unsigned integer"; an ID that does not exist fails with "explicit
  export mesh asset does not exist".

`export-gltf/export-glb` take no mesh ID; they export the whole project scene.
They fail with "error: project has no mesh assets" on an empty project.

### CLI output paths

Every CLI export takes the output file path **explicitly as the last argument**.
There is no default exports directory and no auto-naming; the directory must
already exist (exports open the target directly and fail if it cannot be
opened). Confirming an existing file name overwrites it via atomic replace.

### Safe PowerShell examples

Replace the placeholder paths with your own. Use new output names for anything
you care about.

```powershell
# Import an OBJ into a NEW project (target must not be a project you want to keep)
& "C:\path\to\cartographer_cli.exe" import-obj `
    "C:\path\to\meshes\block.obj" `
    "C:\path\to\projects\block-imported.carto"

# Export the only mesh in a project to OBJ
& "C:\path\to\cartographer_cli.exe" export-obj `
    "C:\path\to\projects\first-block.carto" `
    "C:\path\to\exports\first-block.obj"

# Export mesh asset 2 from a multi-mesh project to STL (binary STL output)
& "C:\path\to\cartographer_cli.exe" export-stl `
    "C:\path\to\projects\scene.carto" 2 `
    "C:\path\to\exports\asset2.stl"

# Export the whole project as glTF / GLB
& "C:\path\to\cartographer_cli.exe" export-gltf `
    "C:\path\to\projects\scene.carto" `
    "C:\path\to\exports\scene.gltf"
& "C:\path\to\cartographer_cli.exe" export-glb `
    "C:\path\to\projects\scene.carto" `
    "C:\path\to\exports\scene.glb"
```

Each successful CLI command prints a receipt line (`imported … journaled=true`,
`exported … vertices=… triangles=…`) followed by any warnings, one per line.

## OBJ: what works and what does not

The OBJ importer (`libs/io/src/obj.cpp`) is deliberately strict.

**Supported records:** `v` (positions), `vt` (2D texture coordinates, optional
third component accepted), `vn` (non-zero normals), `f` (polygon faces of any
supported arity), and the Cartographer comment markers described below. Faces
are admitted as polygons and triangulated only for interchange reporting.

**Everything else is ignored with a warning**, one per record, for example:

```text
warning: ignored unsupported OBJ record 'usemtl' at line 42
```

This covers the usual OBJ extras: `mtllib`, `usemtl`, `o`, `g`, `s`, `l`, `p`,
curves, and so on. The warning list caps at 10 000 entries, after which the
import fails with "OBJ unsupported-record warning limit exceeded". The file
size cap is **128 MiB**. If you hit the partial-UV or partial-normal errors
below, [Troubleshooting](troubleshooting.md#9-obj-import-warnings-partial-uvs-or-normals)
walks through what to change in the source file.

**Hard errors (import fails, nothing is created):**

- A face whose corners mix UV and no-UV references ("OBJ face mixes corners
  with and without texture coordinates"), or mixed normals likewise.
- **Partial UV coverage** across the file: "OBJ has partial texture-coordinate
  coverage; Cartographer requires UVs for every face". UVs are all-or-nothing.
- **Partial normal coverage** likewise: "OBJ has partial normal coverage;
  Cartographer requires normals for every face". Normals are also
  all-or-nothing, and a `vn` record with a zero/invalid vector is rejected.
- Malformed vertices, UVs, normals, or face references (bad indices, non-finite
  values) are rejected with a line-numbered error.

When UVs are present they become the mesh's active render UV set; when normals
are present they become a custom corner normal layer.

**MTL is not imported.** `mtllib`/`usemtl` produce only the ignored-record
warning above — no material is created and nothing is assigned. Material
information survives an OBJ round trip only through the Cartographer region
comments below, which carry numeric region IDs, not materials.

## Cartographer face material regions in OBJ

Cartographer extends OBJ with comment markers so face **material region**
identity survives interchange:

```text
# CARTOGRAPHER_FACE_MATERIAL_REGIONS 1 condition.material_region
# CARTOGRAPHER_FACE_MATERIAL 1 1
# CARTOGRAPHER_FACE_MATERIAL 2 1
```

- `# CARTOGRAPHER_FACE_MATERIAL_REGIONS 1 <layer-id>` is the table header; it
  must appear first and once, with version `1` and a layer ID of either
  `condition.material_region` or `cartographer.material_slot`. A malformed
  header, a second header, or a stray table entry without the header fails the
  import.
- `# CARTOGRAPHER_FACE_MATERIAL <face-id> <region-id>` assigns a **non-zero**
  region ID to one face (1-based face numbering). The table must cover **every**
  face exactly once; a missing face, a zero region, or a duplicate entry fails
  the import with "OBJ material-region table does not cover every admitted
  face" or "malformed or duplicated".

On import the table becomes a face-domain `uint32` attribute layer under the
named layer ID. On export, Cartographer writes these markers from the mesh's
`condition.material_region` (or `cartographer.material_slot`) face layer — one
`# CARTOGRAPHER_FACE_MATERIAL` record per triangle of the triangulated output,
right after the leading "polygon topology is triangulated for interchange"
comment.

### First Block, concretely

The checked-in example `examples/first-block/first-block.obj` uses exactly this
mechanism. Its header reads:

```text
# CARTOGRAPHER_FACE_MATERIAL_REGIONS 1 condition.material_region
# CARTOGRAPHER_FACE_MATERIAL 1 1
# CARTOGRAPHER_FACE_MATERIAL 2 1
# CARTOGRAPHER_FACE_MATERIAL 3 1
# CARTOGRAPHER_FACE_MATERIAL 4 1
# CARTOGRAPHER_FACE_MATERIAL 5 1
# CARTOGRAPHER_FACE_MATERIAL 6 1
```

All six quad faces (12 triangles) declare region `1`. Importing that OBJ
therefore yields a project whose single mesh carries a `condition.material_region`
face layer — which is why the First Block README can tell you to create a
material with **Region ID 1** and assign it. Plain OBJ readers simply ignore
these comments.

## Materials vs material regions

Keep two ideas separate; the baseline treats them as different layers of the
model:

- A **material** is a project asset (for example a PBR material source you
  create in the Material instrument) with real shading parameters.
- A **material region** is just a **non-zero numeric ID per face** — a slot
  that says "faces tagged `1` belong together". It carries no color, texture,
  or shader data of its own.

A region ID with no matching material renders with the default material; a
material assigned to a region colors every face tagged with that ID. Empty
projects and newly created boxes have **no materials and no face
material-region layer at all** — a region layer only appears when something
(like the First Block OBJ import) provides one.

## Export loss warnings

Exports succeed with data loss rather than fail, and say so. Treat every
warning line as a real loss you should know about:

- **OBJ export** always reports a summary such as: "OBJ export includes
  positions and triangulated faces, Cartographer face material-region IDs;
  material assets/assignments, additional UV sets, and unsupported attributes
  are omitted". Polygons are triangulated in the output (`f` records are
  triangles), and only the active render UV set and custom corner normals are
  written. A separate warning appears when the mesh has UV sets but none is
  active for rendering ("UV payload was omitted because no UV set is active for
  rendering").
- **PLY export** preserves polygon topology but warns: "PLY preserves polygon
  topology; normals, UVs, materials, and tangents are not included". The PLY
  importer likewise reports "PLY normals, UVs, materials, and tangents were not
  imported".
- **STL export** warns: "STL is triangle-only and does not preserve names,
  polygon topology, UVs, materials, or tangents". Both STL importers ignore
  facet normals and recompute them from topology; binary big-endian PLY is
  rejected, and PLY files must contain `vertex` (with `x`/`y`/`z`) and `face`
  (with a `vertex_indices`/`vertex_index` list) elements — other elements and
  properties are unsupported and fail the import.
- **glTF / GLB export** at this baseline writes geometry (positions, indices,
  and normals, plus UVs and generated tangents when the mesh has an active
  render UV set) plus scene/node structure, but **no material channels**.
  Typical warnings: "Cartographer glTF export does not
  include material channels; consumers must use their default material path",
  "Cartographer glTF export preserves condition.material_region face identity
  in mesh extras; visual material realization remains consumer-owned", and
  "float64 authoring positions and normals are narrowed to glTF float32". Do
  not expect materials to survive a `.carto` → glTF/GLB round trip in this
  build.

Because of these losses, a `.carto` project is always the authoritative copy.
Treat OBJ/PLY/STL/glTF exports as interchange snapshots, not backups.

## Related guides

- [First Block example](../../examples/first-block/README.md) — the small
  practice project used above, including how to
  [rebuild it from source](../../examples/first-block/README.md#rebuild-without-downloads)
  with `generate_example.py` and the CLI.
- [Alpha first-use blockers](alpha-blockers.md) — known baseline failures and
  acceptance gaps that can look like import/export bugs.
- [Troubleshooting and recovery](troubleshooting.md) — symptom-to-fix guidance
  for disabled exports, OBJ import warnings, ambiguous export scope, schema
  errors, and journal recovery.
- Project format notes: current source schema is **35**, and the reader accepts
  schema versions **1 through 35**. Saving an older project (for example the
  schema-34 First Block) upgrades it to the current format on first save.
