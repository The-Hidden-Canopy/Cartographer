# First Block example validation (headless, CLI 0.1.0)

> Historical handoff from 2026-10-28, recovered during worktree consolidation. Workstation paths use placeholders. UI labels, source line references and unimplemented proposals describe that dated candidate. They are not verification of the current application. See [the current texturing verification](../verification/texturing-workflow.md) and [integration ledger](../verification/worktree-integration-20261116.md).


Date: 2026-10-28. Worktree: `<checkout>/.worktrees/first-use-20261028` at `fae4f7d95259ba38e79edc94ae12275df4161cd4`.

## What this is — and is not

All checks below were run **headless** against the pre-existing CLI at
`<checkout>/build/d3d12/apps/cli/cartographer_cli.exe`
(`cartographer_cli --version` → `0.1.0`, binary dated 2026-10-26, 12,642,816 bytes).
This CLI is the **older, pre-existing 0.1.0 tool — it is not the fae4f7d candidate**
under review, and it does not read or exercise the candidate's schema-35 code path
(its own output is schema 34; the candidate source reads v1–v35). **No GUI or native
application was launched, built, or runtime-accepted in this lane.** Every result here
is a CLI exit code and text output only.

All artifacts were written to the git-ignored directory
`build/first-use-validation-20261028/` (matched by `.gitignore` rule `/build/`).
No checked-in example, project, or source file was modified or overwritten.

The commands below use this shell variable:

```bash
CLI=/e/HiddenCanopy/Cartographer/build/d3d12/apps/cli/cartographer_cli.exe
```

## 1. Validate the checked-in project

Command (run from the worktree root):

```bash
"$CLI" validate examples/first-block/first-block.carto
```

Observed output (exit code 0):

```
valid "examples/first-block/first-block.carto"
schema=34 objects=1 meshes=1 revision=4
```

Consistent with the manifest: schema 34, 1 object, 8 vertices, 6 quads / 12
triangles, region 1 on all faces, no material.

## 2. Export OBJ with the one-mesh command

Command:

```bash
"$CLI" export-obj examples/first-block/first-block.carto 1 build/first-use-validation-20261028/first-block-export.obj
```

Observed output (exit code 0):

```
exported "build/first-use-validation-20261028/first-block-export.obj" mesh_asset=1 vertices=8 triangles=12
warning: OBJ export includes positions and triangulated faces, Cartographer face material-region IDs; material assets/assignments, additional UV sets, and unsupported attributes are omitted
```

Inspection of the exported file confirms:

- Exactly 8 `v` lines with the original coordinates (2 × 0.5 × 1.5 m block).
- 12 `f` lines: the 6 quads are triangulated (fan split, e.g. `f 1 4 3` / `f 1 3 2`).
- Region metadata is present and correct: header
  `# CARTOGRAPHER_FACE_MATERIAL_REGIONS 1 condition.material_region` followed by
  12 `# CARTOGRAPHER_FACE_MATERIAL <n> 1` comments — one per triangulated face, all region 1.

The export warning already states the known limitation: material assets/assignments and
UV sets are omitted (this project has none, so nothing user-visible was lost at this step).

## 3. Reimport the exported OBJ into a new project and validate

Commands:

```bash
"$CLI" import-obj build/first-use-validation-20261028/first-block-export.obj build/first-use-validation-20261028/reimported.carto
"$CLI" validate build/first-use-validation-20261028/reimported.carto
```

Observed output (exit codes 0 / 0):

```
imported "build/first-use-validation-20261028/first-block-export.obj" -> "build/first-use-validation-20261028/reimported.carto" vertices=8 faces=12 triangles=12 journaled=true
valid "build/first-use-validation-20261028/reimported.carto"
schema=34 objects=1 meshes=1 revision=4
```

Round-trip result: 8 vertices and all-1 face regions survive; the project validates.
**Observed limitation, not patched around:** the round-trip changes topology — the
reimported project has 12 triangular `FACE` records instead of the original 6 quads
(`FACES 6` → `FACES 12`, `DOMAIN_COUNT 3 6` → `DOMAIN_COUNT 3 12`). The material-region
layer is preserved (12 `ATTRIBUTE_VALUE 1` entries). The reimported project also gets a
generated namespace (`cartographer.project.511a551d79504f382d6d1ed2a48f255f`) and name
`first-block-export`. This is OBJ interchange behavior of CLI 0.1.0, not a defect in the
checked-in example.

## 4. Export glTF and confirm output plus material-loss warnings

Command:

```bash
"$CLI" export-gltf examples/first-block/first-block.carto build/first-use-validation-20261028/first-block.gltf
```

Observed output (exit code 0):

```
exported "build/first-use-validation-20261028/first-block.gltf" profile=cartographer-gltf-v1 container=gltf-json objects=1 meshes=1 vertices=8 triangles=12 index_bytes=72 index_type=uint16
warning: Cartographer authoring has no UV, tangent, or material channels; consumers must use their default material path
warning: Cartographer glTF export preserves condition.material_region face identity in mesh extras; visual material realization remains consumer-owned
warning: float64 authoring positions and normals are narrowed to glTF float32
```

Inspection of the 1,726-byte `first-block.gltf` (JSON container, 1 buffer of binary
data, 3 buffer views, 3 accessors, POSITION accessor count = 8):

- **Material-loss warnings confirmed:** `materials` is absent from the document and the
  primitive has no `material` index; the CLI warns that there are no UV/tangent/material
  channels and consumers must use their default material path. This project has no
  material by design, so the export is faithful; the warnings accurately describe the
  lossy surface for projects that do have materials.
- **Region metadata survives:** `meshes[0].primitives[0].extras.condition_material_region`
  lists the 6 original faces, each `region: 1`, with layer `condition.material_region`.
  The primitive `extras` also carry `cartographer_mesh_id: 1`.
- Positions are float32 (float64→float32 narrowing, as warned).

## 5. Regenerate the example twice and compare

Commands (run from `examples/first-block/`):

```bash
python generate_example.py --cli "$CLI" ../../build/first-use-validation-20261028/regen-a
python generate_example.py --cli "$CLI" ../../build/first-use-validation-20261028/regen-b
```

Both runs succeeded (exit code 0). Each run prints the CLI import
(`vertices=8 faces=6 triangles=12 journaled=true`), the CLI validation
(`valid ... schema=34 objects=1 meshes=1 revision=4`), and
`Created and validated First Block in <abs path>`.

SHA-256 comparison (`build/first-use-validation-20261028/`, computed with Python's
`hashlib.sha256`):

| File | Bytes | SHA-256 | regen-A == regen-B | regen-A == checked-in |
| --- | --- | --- | --- | --- |
| `first-block.carto` | 2579 | `e8c3a15fe7f8274a0eab0f7bcfcaf037a6cba09dd8db69efd15802d25442936f` | yes | yes |
| `first-block.obj` | 607 | `01ecf3fe69a8451f676492111b5981de5ba9a0b3d49cf621c5ab8cc2d52cc012` | yes | yes |
| `manifest.json` | 526 | `77e41a26205e588abf6ec0e3606c23a7faff0157e2d5d433ddecf68cacb4f7f3` | yes | n/a (not checked in) |

The regenerated `first-block.carto` and `first-block.obj` are byte-identical to the
checked-in files and to each other, so `generate_example.py` is deterministic across
runs and reproduces the manifest hashes in `examples/first-block/manifest.json`. The
regenerated manifest agrees with the checked-in one on
`generator_cli_version 0.1.0`, `project_format CARTOGRAPHER_PROJECT 34`, and both file
hashes.

## Limitations and honest caveats

- The validating/importing/exporting tool is the **pre-existing CLI 0.1.0**, not the
  fae4f7d candidate. These results say nothing about candidate code, the schema-35
  read/upgrade path, or the Vulkan workbench (`apps/desktop`); **no GUI step passed or
  was attempted** in this lane.
- The CLI validates and writes schema 34; the candidate source is schema 35. Nothing
  here exercised an upgrade-on-save.
- OBJ interchange triangulates quads, so an export→import cycle converts 6 quads into
  12 triangles (recorded above, not worked around).
- glTF export carries no visual material and no UVs/tangents; region identity is
  preserved only in mesh `extras`.
- Native export's saved-and-unmodified precondition was not tested (no native runtime
  was launched); these CLI exports accept the file as-is.
- All intermediate artifacts remain in the ignored directory
  `build/first-use-validation-20261028/` for re-verification.
