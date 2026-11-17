# First-use troubleshooting and recovery

> Historical handoff from 2026-10-28, recovered during worktree consolidation. Workstation paths use placeholders. UI labels, source line references and unimplemented proposals describe that dated candidate. They are not verification of the current application. See [the current texturing verification](../verification/texturing-workflow.md) and [integration ledger](../verification/worktree-integration-20261116.md).


- **Source revision:** `fae4f7d95259ba38e79edc94ae12275df4161cd4`
- **Worktree:** `<checkout>/.worktrees/first-use-20261028`
- **Date:** 2026-10-28
- **Scope:** the Vulkan desktop workbench (`apps/desktop`). The D3D12 shell
  (`apps/d3d12_desktop`) is a separate target and is not covered here.
- **Status:** every label and error string below is code-derived from the pinned
  revision. No candidate build was launched; nothing here is a runtime/native
  acceptance result. See [evidence.md](evidence.md) for the evidence policy.
- Companion docs: [controls.md](controls.md) (bindings and labels),
  [in-app-help.md](in-app-help.md) (onboarding copy), [alpha-blockers.md](alpha-blockers.md)
  (known gaps).

## Read this first: what Cartographer does *not* do

These three facts explain most "data loss" scares. There is no hidden safety
net anywhere else in this build.

1. **No autosave.** Saving happens only when you choose **File > Save**
   (`Ctrl+S`) or **File > Save As…** (`apps/desktop/main.cpp:6264-6268`).
2. **No automatic recovery on ordinary open.** Opening a saved project loads the
   `.carto` as it is on disk. The application layer has an opt-in
   `auto_recover` open option (`libs/application/include/carto/application/application.hpp:895`,
   default `false`), and the desktop shell never sets it — recovery is always
   the explicit **File > Recover latest journal snapshot** command
   (`main.cpp:6270`).
3. **No previous-version backup.** Saving atomically replaces the `.carto`;
   the old bytes are gone. The journal is *not* a guaranteed backup (see
   Recovery below).

Practical consequence: **save early, save after meaningful steps, and keep your
own copies** of anything you cannot afford to lose.

## Where things live on disk

| Path | What it is |
|------|------------|
| `C:\...\MyProject.carto` | The project. Plain-text `CARTOGRAPHER_PROJECT <schema>` header plus records (`libs/project/src/project.cpp`). Project truth is only here — UI prefs never enter it. |
| `C:\...\MyProject.carto.journal` | Recovery journal, created next to the project once a saved project is mutated (`libs/application/src/application.cpp:792-795`, `1870-1881`). Append-only, revision-bound, hash-chained (`libs/journal/include/carto/journal/journal.hpp:36-37`). |
| `C:\...\MyProject.carto.lock` | Empty OS-level lock file created beside the project during locked operations (`libs/project/src/file_lock.cpp:76-119`). The lock is a Windows byte-range lock, released automatically when the holding process exits. The journal keeps its own `MyProject.carto.journal.lock` sidecar on the same scheme (`libs/journal/src/journal.cpp:135-160`). |
| `C:\...\MyProject.carto.tmp-<ticks>-<thread>-<n>` | Sibling temporary file used during atomic save (`libs/project/src/project.cpp:726-735`). Normally renamed over the `.carto` or cleaned up on failure; a leftover after a crash is inert. |
| `%LOCALAPPDATA%\Cartographer\workspace.prefs` | User UI preferences (theme, density, operator mode), created on demand (`apps/desktop/main.cpp:1697-1714`). |
| `%LOCALAPPDATA%\Cartographer\workbench.prefs` | Workbench layout/flow preferences beside `workspace.prefs` (`main.cpp:1715-1721`). |
| `%LOCALAPPDATA%\Cartographer\workspace-docking.ini` | Dock topology and floating-window layout, stored beside `workbench.prefs`, never in the project (`main.cpp:2449-2454`). |
| `%LOCALAPPDATA%\Cartographer\market-library\` | Local market package library (`main.cpp:1722-1731`). |

If the UI layout ever looks wrong after an update, these four `%LOCALAPPDATA%`
files are the only local state — your project is unaffected. To restore the
default arrangement in-app, run **Ctrl+K > Reset Workspace**
(`workspace.reset`, `libs/ui/src/ui.cpp:12111`) or switch flows with
`Ctrl+Shift+1`–`4`.

## Symptom → fix

### 1. The viewport is blank / empty on first run

Expected, three common causes:

- **New projects start empty.** An unsaved "Untitled" project has zero objects;
  the viewport shows **"No compiled geometry"** (`main.cpp:8338`). Nothing is
  broken.
- **The default Focus flow hides the creation instruments** (see §2). The
  viewport itself is still live.
- **You panned or zoomed away from the scene.** The workbench is a 2D
  orthographic plan view; there is no 3D orbit and no separate "reset camera".

**Fix:** press **`Home`** or the **Frame all** button at the top of the
viewport (`main.cpp:6865-6876`). This fits the scene bounds. Arrow keys,
`< ^ v >` buttons, right/middle-drag, or `Space`/`Alt` + drag pan; the wheel
zooms (details in
[controls.md](controls.md#viewport-navigation-2d-plan-panzoom)). If the scene
genuinely cannot fit, the viewport warns: **"Frame all could not fit this
scene within the supported view range."** (`main.cpp:8336`) — zoom out with
the wheel and frame again.

### 2. I cannot find any way to create a box

The first-run **Focus** workbench flow intentionally hides the creation
instruments; the header shows only the work surface
(`main.cpp:20727`).

**Fix:** open the command palette with **`Ctrl+K`** (or the **Command palette
Ctrl+K** / **Commands** button in the header, `main.cpp:5340`) and run
**Create Box** (`create.box`, `libs/ui/src/ui.cpp:12107`). The palette filters
as you type; it also exposes **Create Plane**, **Reset Workspace**, and flow
switching. To get the instruments back permanently, switch flows with
**`Ctrl+Shift+2` (Author)**, **`Ctrl+Shift+3` (Inspect)**, or **`Ctrl+Shift+4`
(Draft)**; **`Ctrl+Shift+1`** returns to Focus (`main.cpp:19909-19914`).

### 3. Material assignment fails on a new box

Symptoms, in order of how you meet them:

- The Material instrument shows **"No material sources in this project."**
  (`main.cpp:10050`). New projects, new boxes, and the First Block example ship
  with no materials. **Fix:** type a name and choose **Create PBR source**
  (`main.cpp:10750`).
- **"Assign selected material"** stays disabled until a material is selected
  and the **Region ID** is non-zero (`main.cpp:10685-10687`); when the region
  already holds a variant, the same button reads **"Replace variant with base
  material"** (`main.cpp:10687-10690`).
- After you press it, the action can fail with:
  **"material-region assignment references a missing authored region"**
  (`libs/project/src/project.cpp:3106-3113`).

That last error is the important one. Materials bind to **authored material
regions** — a per-face `uint32` layer named `condition.material_region` (or
`cartographer.material_slot`) on the mesh
(`libs/project/src/project.cpp:766-767`). A mesh only "declares" a region if at
least one face carries that region ID. **Boxes and planes created in-app have
no material-region layer at all**, so *no* Region ID exists to bind to, and the
assignment is rejected at project validation.

**Workaround for first use — use the First Block example.** The shipped
`examples/first-block/first-block.obj` declares
`# CARTOGRAPHER_FACE_MATERIAL_REGIONS 1 condition.material_region` and maps
region **1** onto all six faces (`# CARTOGRAPHER_FACE_MATERIAL 1..6 1`), so
open `examples/first-block/first-block.carto` (or import its OBJ), select the
object, enter **Region ID 1**, and **Assign selected material** — that region
genuinely exists. Region 1 is the sensible default everywhere: one region
covering the whole mesh.

Related exact messages and their meaning:

- **"material region already uses the selected material"** — the binding you
  asked for already exists; nothing to do (`libs/application/src/application.cpp:10111`).
- **"No material is assigned to this region."** — informational; the Region ID
  is valid but unbound. Select a material and assign (`main.cpp:10662`).
- **"material-region assignment references a missing material"** — the selected
  material ID no longer exists (e.g. deleted); pick a current material
  (`libs/application/src/application.cpp:10097`).

### 4. "Project locked by another process" / save, recovery, or open fails with a lock error

Exact diagnostic text: **"unable to acquire file lock"** (project locks) or
**"unable to acquire journal lock"** (journal sidecar), surfaced with contexts
like **"project read lock"**, **"project write lock"**,
**"project mutation lock"**, **"project recovery lock"**, or
**"recovery inspection lock"** (`libs/project/src/file_lock.cpp:107-119`,
`libs/application/src/application.cpp:4044-4046, 4146-4148, 4210-4212, 1876-1879`,
`libs/journal/src/journal.cpp:169-180`).

Cartographer takes an exclusive OS-level byte-range lock on
`MyProject.carto.lock` (and `MyProject.carto.journal.lock` for journal writes)
for the short duration of saves, journaled mutations, recovery, and some
inspections. If another Cartographer process is touching the same project at
that instant, the operation fails rather than risking two writers.

**Fix:** close the *other* Cartographer instance that has this project open,
then retry. **Do not delete the `.lock` file** — the lock is held by the OS on
behalf of a live process, not by the file's existence; deleting it fixes
nothing and can mask a real second writer. A `.lock` file left behind after a
crash is harmless: a new lock acquisition on it succeeds once no process holds
the range.

### 5. Wrong, corrupt, or unsupported project schema

The first line of every `.carto` reads `CARTOGRAPHER_PROJECT <n>`. This build
writes **schema 35** and reads **versions 1 through 35**
(`libs/project/include/carto/project/project.hpp:326-327`); the shipped First
Block example is schema 34 and opens fine.

Exact message: **"unsupported Cartographer project schema version"**
(`libs/project/src/project.cpp:5820-5823`), shown with an **"open project"**
context when **File > Open** rejects the file.

- **Version number > 35:** the file was written by a *newer* Cartographer.
  Open it with that newer build; this build will not guess at unknown records.
- **Version number < 1, or the file is not a Cartographer project at all**
  (wrong file picked, truncated download, bytes overwritten): treat it as
  corrupt — see Recovery below. If the first line is not even
  `CARTOGRAPHER_PROJECT`, you opened the wrong file.
- **Truncated or torn file:** the reader fails closed with a parse/validation
  diagnostic naming the offending record.

Do **not** hand-edit the `.carto` to change the version number; schema fields
differ per version and the loader validates every record.

### 6. Export is disabled, or says "Save the .carto project before exporting…"

Native export is gated on a **saved, unmodified** project:
`can_export = snapshot.project_path.has_value() && !snapshot.dirty`
(`apps/desktop/main.cpp:6290`). **Export glTF / GLB / OBJ / PLY / STL** stay
disabled until both hold (`main.cpp:6307-6319`).

If you reach export through a route that bypasses the disabled menu, the shell
tells you exactly why (`main.cpp:4771-4777, 4952-4958`):

- **"Save the .carto project before exporting a glTF asset."** — no file bound yet.
- **"Save the .carto project before exporting a mesh asset."** — same, for OBJ/PLY/STL.
- **"Save the project before exporting so the derived asset has a durable source revision."**
  — the project has *unsaved changes* (dirty).

**Fix in all cases:** **File > Save** (`Ctrl+S`), then export again. This is a
deliberate provenance rule, not a fault.

### 7. Closing with unsaved changes: Discard vs Keep Editing

Closing the window (or running **File > New** / **Open** / **Recover** on a
dirty project) opens the modal **"Discard Unsaved Changes"**
(`main.cpp:5117-5148`):

> This project has unsaved changes. Discard them and continue?

- **Keep Editing** returns you to the project, changes intact. Choosing it is
  always safe.
- **Discard** continues the pending action — closing the app (unsaved work is
  gone) or abandoning the project for the new/open/recovered one.

Because there is no autosave and no previous-version backup, **Discard is
irreversible**. If in doubt, choose **Keep Editing**, then **Save As…** a copy
first.

### 8. "Cartographer Vulkan unavailable" at startup

On launch the shell probes the Vulkan runtime and shows **"Cartographer
Vulkan unavailable"** with a specific reason, then exits (return code 2;
`apps/desktop/main.cpp:20815-20824`, `libs/vulkan/src/runtime.cpp:215-300`).
Observed reasons include:

- **"Cartographer native Vulkan runtime requires Vulkan 1.1"** — the installed
  driver/API is too old (`runtime.cpp:229`).
- **"Vulkan instance creation failed: …"** — no usable Vulkan loader/driver
  (common on fresh VMs, remote desktops without GPU, or missing GPU drivers).
- **"Vulkan reported no physical device"** / **"Vulkan physical-device
  enumeration failed"** — no adapter passed the probe (`runtime.cpp:257, 265`).

**Fix:** install/update your GPU vendor's driver (or the Vulkan runtime on a
VM). There is **no software/CPU rendering fallback** in this build — if the
probe fails, the workbench cannot start, and editing the preference files will
not help (they contain UI state only).

### 9. OBJ import warnings, partial UVs or normals

**File > Import Mesh…** (`Ctrl+I`) accepts **OBJ, PLY, and STL** only
(`main.cpp:2998-3014`); the picker filter says "Supported Meshes
(\*.obj;\*.ply;\*.stl)", and any other extension is rejected with **"Choose an
OBJ, PLY, or STL mesh file."** glTF is an export format in this build, not an
import target. A successful import reports through **"Cartographer mesh import
warnings"** when there is anything to note (`main.cpp:3036-3040`):

- **"ignored unsupported OBJ record '<tag>' at line <n>"** — records such as
  groups/objects (`o`, `g`), materials (`usemtl`), or smoothing are not
  imported; the geometry still comes in. Note the consequence: **a multi-object
  OBJ is merged into a single Cartographer mesh** — there is no per-object
  split on import.
- **"isolated OBJ face <n> by duplicating vertices across a conflicting
  directed edge"** — the importer repaired inconsistent winding/topology by
  duplicating vertices; the mesh is intact but no longer shares those vertices.
- **"OBJ has partial texture-coordinate coverage; Cartographer requires UVs
  for every face"** and **"OBJ has partial normal coverage; Cartographer
  requires normals for every face"** — *hard rejections*, not warnings: if some
  faces reference `vt`/`vn` and others do not, the whole import fails
  (`libs/io/src/obj.cpp:733-742`). **Fix:** in your exporter or DCC, either
  export UVs (resp. normals) for every face or for none; an OBJ with *no* UVs
  at all imports fine and simply carries no UV layer.
- **"OBJ input exceeds the Cartographer import limit of 128 MiB"** and the
  vertex/face/record-count limits — split the source file.

If a supported file still fails, the **"Cartographer mesh import failed"**
message names the validation and line that rejected it; fix the file and import
again.

### 10. Export scope ambiguous / multiple meshes

Mesh export (OBJ/PLY/STL, and glTF scope) resolves to **exactly one reusable
mesh asset** (`libs/production/src/preparation_scope.cpp:242-256`). If your
selection maps to zero or several, you get
(`main.cpp:5017-5023`):

- **"Select objects that resolve to exactly one reusable mesh asset."**
  (title: **"Cartographer export scope ambiguous"**) — your selection spans
  multiple mesh assets. **Fix:** select one object (or instances that share one
  mesh) and export again.
- With nothing selected, the whole project must resolve to a single mesh asset;
  a project containing several distinct meshes has no unambiguous export scope.
  Export one object at a time.

### 11. Missing material / texture / package dependencies

- **"material texture binding references a missing texture source"** and
  **"material graph texture sample references a missing texture source"**
  (`libs/project/src/material_catalog.cpp:798, 866`) — a material points at a
  texture that is not in the project. **Import Texture Source…** in the
  Material instrument registers textures (PNG/JPG/JPEG/TGA/DDS/EXR; bounded
  header inspection — pixels are not decoded or rendered,
  `main.cpp:10019-10026`).
  This button requires a saved project: **"Save this project before importing
  texture sources."** (`main.cpp:10031`).
- **"material texture refers to a missing image or sampler"** — raised while
  reading glTF material data (`libs/io/src/gltf_material_import.cpp:370`); the
  source file references an image that is not embedded/resolvable.
- Market packages: **"dependency package is unavailable: <id>"**,
  **"resolved dependency is missing: <id>"**,
  **"dependency constraints cannot be satisfied: <id>"**, and
  **"market dependency cycle detected at: <id>"**
  (`libs/market/src/market.cpp:1551-1591`). **Fix:** install the missing
  dependency package first via **File > Install data-only market package…**,
  then retry; the local library lives in
  `%LOCALAPPDATA%\Cartographer\market-library\`.

## Recovery: File > Recover latest journal snapshot

What it does: replays the project's `.carto.journal` to the newest verified
snapshot and **writes that state back over the `.carto`**
(`libs/application/src/application.cpp:4140-4165`,
`libs/project/src/recovery.cpp`). It is the right tool after a crash, a torn
save, or a corrupted `.carto` — *provided a pending snapshot exists*.

Hard limits to internalize before you rely on it:

- **The journal is not a guaranteed backup.** It exists only for *saved*
  projects, only entries that passed verification are replayable, and recovery
  stops at the newest verified snapshot. Unsaved session changes were never
  journaled.
- No autosave, no automatic recovery on ordinary open, no previous-version
  backup (see Read this first). If recovery reports no pending snapshot, your
  own copy is the only copy.

### Safe recovery procedure

1. **Copy before you recover.** Make a copy of **both**
   `MyProject.carto` **and** `MyProject.carto.journal` (put them in a backup
   folder). Recovery overwrites the `.carto`, and there is no previous-version
   backup.
2. Open Cartographer and the project (if the `.carto` will not open at all,
   just have the project file path ready).
3. Choose **File > Recover latest journal snapshot** (`main.cpp:6270`).
4. If the current session has unsaved changes, the **"Discard Unsaved
   Changes"** dialog appears first — recovery discards them
   (`main.cpp:5054-5090`). Choose **Discard** to continue (or **Keep Editing**
   to save a copy first).
5. The project is replaced with the newest journaled state and saved.

### What you will see when it cannot recover

- **"No pending journal snapshot is available for this project."**
  (`main.cpp:5077-5078`) — the journal has no recoverable snapshot: the
  project was never saved, the journal is absent/empty, or everything was
  already saved. **There is nothing to recover — restore your own backup.**
- **"Cartographer recovery blocked"** / **"durable recovery evidence is
  blocked"** (`main.cpp:5066-5071`) — the journal failed verification (torn or
  tampered records). Recovery refuses rather than guessing. Copy the project
  and journal aside untouched and file a bug (checklist below).
- **"Cartographer recovery unavailable"** (`main.cpp:5058-5061`) — recovery
  could not even be inspected, usually a lock failure ("recovery inspection
  lock", §4). Close the other instance and retry; if it persists, file a bug.

## Error text → required action (quick table)

| Exact message | Required action |
|---|---|
| "Save the .carto project before exporting a glTF asset." / "…a mesh asset." / "Save the project before exporting so the derived asset has a durable source revision." | Save (`Ctrl+S`), then export. |
| "unsupported Cartographer project schema version" | Version > 35: open in a newer build. Otherwise treat the file as corrupt; try recovery, else restore your backup. |
| "unable to acquire file lock" ("project read/write/mutation/recovery lock") / "unable to acquire journal lock" | Close the other Cartographer instance using this project; retry. Do not delete the `.lock` file. |
| "material-region assignment references a missing authored region" | Use a Region ID actually declared on the mesh (e.g. First Block's region 1); boxes have no regions. |
| "No material sources in this project." | Create one: **Create PBR source** in the Material instrument. |
| "No pending journal snapshot is available for this project." | Nothing to recover; restore your own backup. |
| "Cartographer recovery blocked" / "durable recovery evidence is blocked" | Preserve `.carto` + `.carto.journal` as-is; file a bug. |
| "OBJ has partial texture-coordinate coverage; Cartographer requires UVs for every face" (or normals) | Fix the OBJ so every face has `vt` (resp. `vn`) or none do; re-import. |
| "OBJ input exceeds the Cartographer import limit of 128 MiB" | Split the source mesh/file. |
| "Select objects that resolve to exactly one reusable mesh asset." | Narrow the selection to one mesh asset and export again. |
| "Cartographer Vulkan unavailable" (startup message box) | Update/install GPU drivers or Vulkan runtime; there is no software fallback. |
| "Frame all could not fit this scene within the supported view range." | Zoom out with the wheel, then Frame all / `Home` again. |
| "material texture binding references a missing texture source" | Import the texture (**Import Texture Source…**, saved project required) or rebind the material. |
| "dependency package is unavailable: <id>" / "resolved dependency is missing: <id>" | Install the missing market package, then retry. |

## Information to send with a bug

Copy this checklist into your report and fill what you can:

- [ ] **Exact error text** — the full message from the dialog or the status/action feedback line, including any quoted context (e.g. "unable to acquire file lock: project write lock").
- [ ] **What you were doing** — the command/menu/shortcut (e.g. "File > Recover latest journal snapshot after a power loss"), step by step.
- [ ] **Project state** — saved or never saved; dirty (unsaved changes) or clean; the `.carto` path (folder names are fine, redact if sensitive).
- [ ] **First line of the `.carto`** — the `CARTOGRAPHER_PROJECT <n>` header value (open in a text editor; do not save it from the editor).
- [ ] **Sibling files present?** — does `MyProject.carto.journal` / `MyProject.carto.lock` exist next to the project, and their approximate sizes.
- [ ] **File provenance** — where the project/mesh came from (e.g. "shipped `examples/first-block`", "exported from Blender 4.x as OBJ"), plus the mesh's source format.
- [ ] **Diagnostics** — what the **Diagnostics** surface and the **Problems** tab in the bottom ledger report (error code + message), and whether the shell shows **Native shell** Active or Unavailable (`main.cpp:5473-5477`).
- [ ] **System** — Windows version, GPU model, and driver version (especially for Vulkan-unavailable reports).
- [ ] **Reproducibility** — happens every time / once after a crash; whether a fresh project shows the same behavior.

For recovery-blocking bugs specifically: **attach (or offer) the `.carto` and
`.carto.journal` pair unchanged** — they are the evidence recovery refused to
guess at.

## Honest limitations

- This guide is code-derived against `fae4f7d`; no runtime build was exercised,
  so phrasing like "the dialog says" is verified against source strings, not a
  live screen. Exact strings can drift with UI changes.
- There is **no Help menu, no F1 route, and no onboarding flow** in this
  baseline; this document and its siblings are the substitute.
- Known gaps that look like bugs but are tracked elsewhere: no autosave or
  backups (see [alpha-blockers.md](alpha-blockers.md)), no 3D viewport camera,
  glTF is export-only, no built-in materials, and texture sources are header
  inspection only (no pixel decode/render).
