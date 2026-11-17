# First-use evidence checklist

> Historical handoff from 2026-10-28, recovered during worktree consolidation. Workstation paths use placeholders. UI labels, source line references and unimplemented proposals describe that dated candidate. They are not verification of the current application. See [the current texturing verification](../verification/texturing-workflow.md) and [integration ledger](../verification/worktree-integration-20261116.md).


- **Source revision:** `fae4f7d95259ba38e79edc94ae12275df4161cd4`
- **Worktree:** `<checkout>/.worktrees/first-use-20261028`
- **Date:** 2026-10-28
- **Status headline:** No candidate build of this revision was launched and no candidate step is marked passed. Everything below is either observed against a pre-existing binary, derived from static source reading, or explicitly untested.

## How to read this checklist

- **Observed passes** — commands actually run, with their real outputs. These used a pre-existing released CLI binary, not a candidate build of `fae4f7d`, and do not constitute native acceptance of any candidate.
- **Code-derived findings/failures** — conclusions from reading source at the pinned revision. They are labeled with the responsible surface.
- **Untested steps** — first-use journey steps with no runtime evidence. Each row is explicit so nothing is silently assumed.

Headless re-validation of the example asset was run with the pre-existing CLI
0.1.0 binary and is documented in
[example-validation.md](example-validation.md); it did not execute any candidate
source. The observed passes below summarize its results. Full commands, exact
outputs, and artifact paths live there.

## Observed passes (pre-existing CLI, not the candidate)

All runs used the pre-existing `<checkout>/build/d3d12/apps/cli/cartographer_cli.exe`,
version 0.1.0 (binary dated 2026-10-26, 12,642,816 bytes) — not a build of `fae4f7d`.
Artifacts were written to the git-ignored `build/first-use-validation-20261028/`.

| # | Observation | Evidence |
|---|-------------|----------|
| O1 | Example regenerated twice via `generate_example.py --cli`; both runs exit 0 (`vertices=8 faces=6 triangles=12 journaled=true`, `valid ... schema=34 objects=1 meshes=1 revision=4`). The regenerated `first-block.carto` (2,579 bytes) and `first-block.obj` (607 bytes) are byte-identical across runs and to the checked-in files: SHA-256 `e8c3a15fe7f8274a0eab0f7bcfcaf037a6cba09dd8db69efd15802d25442936f` and `01ecf3fe69a8451f676492111b5981de5ba9a0b3d49cf621c5ab8cc2d52cc012`, matching `examples/first-block/manifest.json`. Generation is deterministic. | [example-validation.md](example-validation.md) §5; `examples/first-block/manifest.json`. |
| O2 | Checked-in project validates: `validate` exit 0 — `valid`, schema 34, objects=1, meshes=1, revision=4; consistent with the manifest (8 vertices, 6 quads / 12 triangles, region 1 on all faces, no material). | [example-validation.md](example-validation.md) §1; `examples/first-block/first-block.carto`, `manifest.json`, `PROVENANCE.md`. |
| O3 | OBJ export passed (`export-obj` exit 0): `vertices=8 triangles=12`; the exported file has 8 `v` lines, 12 triangulated `f` lines, and correct region metadata (12 `# CARTOGRAPHER_FACE_MATERIAL <n> 1` comments, all region 1), with the CLI's standing warning that material assets/assignments, extra UV sets, and unsupported attributes are omitted. | [example-validation.md](example-validation.md) §2. |
| O4 | Round-trip reimport passed (`import-obj` then `validate`, both exit 0): `vertices=8 faces=12 triangles=12`, `valid`, schema 34. Observed limitation: the round-trip changes topology — 6 quads become 12 triangular `FACE` records (`FACES 6` → `FACES 12`), and the reimported project gets a generated namespace and the name `first-block-export`. The region layer survives (12 `ATTRIBUTE_VALUE 1` entries). This is CLI 0.1.0 OBJ interchange behavior, not a defect in the checked-in example. | [example-validation.md](example-validation.md) §3. |
| O5 | glTF export passed (`export-gltf` exit 0): `profile=cartographer-gltf-v1`, 1 object / 1 mesh, `vertices=8 triangles=12 index_bytes=72 index_type=uint16`. The 1,726-byte JSON output preserves region identity only in `meshes[0].primitives[0].extras.condition_material_region` (6 faces, all region 1) and omits `materials` and UV/tangent channels; the three CLI warnings (no UV/tangent/material channels, consumer-owned material realization, float64→float32 narrowing) accurately describe this lossy surface. | [example-validation.md](example-validation.md) §4. |

Limits: the CLI used is the pre-existing 0.1.0 binary, not a build of `fae4f7d`
(current source schema is 35; the reader accepts v1–v35, so the schema-34 example
is expected to open, but that has not been exercised at runtime). Native behavior
is code-checked, not candidate runtime-accepted. No candidate GUI was launched —
no build or approved interaction window was available — so nothing above is a
native acceptance result.

## Code-derived findings / static mismatches

Findings below come from reading source at `fae4f7d`; none has runtime confirmation.

| Finding | Responsible surface |
|---------|--------------------|
| No Help or onboarding content exists in this baseline; first-launch guidance is absent. | `apps/desktop/main.cpp` (no help/onboarding UI); docs set. |
| Default first-run Focus flow hides creation instruments, including New Box. Creating a box requires Ctrl+K > Create Box. | `apps/desktop/main.cpp` (workbench flow `Focus`; `New Box` button at line ~10885 is outside the Focus-visible set; command palette at `Ctrl+K`). |
| No built-in materials ship with the application; empty projects and new boxes start with no materials. | `libs/application`, `apps/desktop/main.cpp` material panels. |
| Primitive boxes have no face material-region layer, so face-level material assignment needs a matching region layer first. | `libs/project` mesh/region model. |
| Material assignment requires a matching region and affects every instance of that region (no per-instance face override in this baseline). | `libs/project`, `libs/application` assignment path. |
| Native export requires a saved, unmodified `.carto` (clean-save gate; unsaved edits block export). | `apps/desktop/main.cpp` export path ("Save the .carto project before exporting"). |
| The close/discard prompt does not offer a Save option (discard only, or cancel). | `apps/desktop/main.cpp` close flow. |
| No autosave and no previous-version backups; recovery is explicit via File > Recover latest journal snapshot. | `apps/desktop/main.cpp:6270`, `libs/project/src/recovery.cpp`. |
| The Vulkan workbench is the plan-only path; the D3D12 shell is the separate desktop target. Vulkan viewport behavior is unverified. | `apps/desktop` vs Vulkan plan docs. |
| glTF has export routes in the UI and CLI, but no user-facing import route: no native File-menu glTF import item and no `import-gltf` CLI command. An application-layer scene import exists in this tree (`libs/application/src/gltf_scene_import.cpp`) but is only reachable through the governed botanical-runtime promotion path, not exposed as a supported user import; the OBJ path is the supported mesh import. | `apps/desktop/main.cpp` (File menu import is OBJ/PLY/STL only; botanical promotion ~line 17436), `apps/cli/main.cpp` (no `import-gltf` dispatch), `libs/application/src/gltf_scene_import.cpp`. |
| Current source schema is 35 and the reader accepts v1–v35, so the schema-34 example is within the readable range. | `docs/format/cartographer-project-v35.md`. |

## Untested journey steps

Every first-use journey step lacks runtime evidence on the candidate. No candidate
step is marked passed, anywhere in this document.

| Step | Status | Notes |
|------|--------|-------|
| Launch the candidate GUI | Untested | No build or approved interaction window was available. |
| Import / open `examples/first-block/first-block.carto` | Untested | Schema 34 expected readable by the v1–v35 reader; not exercised at runtime. |
| Select the object | Untested | |
| Home / frame camera | Untested | |
| Move edit (transform) | Untested | |
| Material create | Untested | No built-in materials exist; creation path unverified. |
| Material assign | Untested | Needs matching region layer; affects every instance — behavior derived from code only. |
| Save | Untested | |
| Close (with and without edits) | Untested | Discard prompt lacks Save — code-derived only. |
| Cold reopen | Untested | |
| Native export | Untested | Clean-save gate code-derived only; no user-facing glTF import route (export-only; application-layer import exists but is not wired to a menu or CLI command). |
| Docking / detach / reset of panels | Untested | |
| Recovery (journal snapshot) | Untested | File > Recover latest journal snapshot exists in source; no autosave/backups. |
| First-launch help / onboarding | Untested | No Help exists in baseline, so there is nothing to test here yet — recorded for completeness. |

## Blockers and follow-ups

- No candidate build or approved GUI interaction window was available in this lane; all runtime steps remain open. Headless re-validation of the example used the pre-existing CLI 0.1.0 binary and did not execute candidate source; it is documented in [example-validation.md](example-validation.md), and O1–O5 above summarize its observed results, none of which is a candidate acceptance result.
- Static mismatches above should be re-checked after any UI lane lands changes, since they were read at `fae4f7d` only.
