# Cartographer first-use startup guide

> Historical handoff from 2026-10-28, recovered during worktree consolidation. Workstation paths use placeholders. UI labels, source line references and unimplemented proposals describe that dated candidate. They are not verification of the current application. See [the current texturing verification](../verification/texturing-workflow.md) and [integration ledger](../verification/worktree-integration-20261116.md).


A beginner walkthrough for the alpha Vulkan workbench: from launch to a saved,
material-assigned project and a clean OBJ export, using the bundled
[First Block example](../../examples/first-block/README.md).

> **Verification status.** Every label, shortcut, and menu path below was
> checked against the application source at commit
> `fae4f7d9` ("Import static GLTF scenes through governed product-bound
> history"). A native click-through of this exact journey on a candidate build
> is **pending** — no candidate GUI was launched for this package, so nothing
> here is runtime-accepted. Where behavior is code-derived rather than observed,
> this guide says so. See [evidence.md](evidence.md) for the checklist and
> [alpha-blockers.md](alpha-blockers.md) for known gaps, including the fact
> that **no in-app Help or onboarding exists in this build** — this document
> is the onboarding. The proposed in-app copy exists only as a spec
> ([in-app-help.md](in-app-help.md)); nothing is wired today
> ([sol-integration-handoff.md](sol-integration-handoff.md)).

## 1. Launch the right application

This source tree provides two native shells. You want the **Vulkan workbench**:

- Run **`cartographer_desktop.exe`** — that is the executable this source tree
  builds and installs. A start-menu or desktop shortcut may label it simply
  "Cartographer"; the reliable check is the window title, which reads
  **"Cartographer | Spatial Intelligence by Design"**.
- Do **not** use the separate D3D12 research shell (`apps/d3d12_desktop` and its
  CLI/viewer executables). It is not the authoring workbench this guide covers.

No saved project is opened automatically: the workbench begins with an
empty, not-yet-saved project.

## 2. Orient: workspace, viewport, and the Focus flow

- The workspace bar lists **Model**, **Sculpt**, **UV**, **Engineering**, and
  more as labeled buttons. Stay in the default **Model** workspace — it is the
  one this guide uses, and object translation (the MOVE handles in section 4)
  is supported there.
- The main viewport is a **2D X/Z plan view** (a top-view projection of the
  ground plane). You pan by dragging the background (or arrow keys) and zoom
  with the wheel; **Home** or the **Frame all** button fits everything in view.
- The first run opens in the **Focus** workbench flow. Focus deliberately
  hides the instruments (Scene, Tools, Material, and the rest) so the viewport
  is uncluttered — which also means creation instruments are not visible on
  day one. Two ways to cope:
  - Open instruments on demand from the **View** menu (e.g. **View > Material
    instrument**), or switch flows with **Ctrl+Shift+2** (Author shows Scene,
    Tools, and Inspector).
  - If panels ever feel lost or misplaced, **View > Reset workspace layout**
    restores the default arrangement.

The save state is never hidden: the work-surface toolbar always shows the
project status text — **Not saved**, **Saved**, or **Modified** — and the
rail's Session readout adds a save-state dot plus an **"Unsaved changes"**
marker whenever the project is dirty. The full binding table lives in
[controls.md](controls.md).

## 3. Create the example asset (File > Import Mesh...)

The exercise asset is the original practice mesh
`examples/first-block/first-block.obj` — one block, 8 vertices, 6 quad faces,
every face already carrying **material region 1**, and no material authored.

1. **File > Import Mesh...** (or **Ctrl+I**).
2. In the file dialog, choose `examples/first-block/first-block.obj`.
3. The block appears as a new object in the project. If you cannot see it,
   press **Home** or click **Frame all** in the viewport header.

The OBJ's `CARTOGRAPHER_FACE_MATERIAL` comments are Cartographer metadata that
survive the round trip; that is what puts region 1 on every face. Import
accepts OBJ, PLY, and STL in this build — see
[import-export.md](import-export.md) for formats and import warnings.

## 4. Select the object

- Make sure the mode combo in the work-surface toolbar reads **Object**
  (shortcut **1**). Object mode is the default.
- Click the block in the viewport, **or** click its row in the **Scene**
  instrument (the outliner lists every object by name and `#id`).
- With the block selected, two labeled handles appear on it: **MOVE X** and
  **MOVE Z**. Drag either handle to slide the block along that axis, exactly as
  the in-view hint says: *"Drag MOVE X / MOVE Z in the view, or set an exact
  position."* (**Ctrl** snaps to the 0.25 m grid; **Shift** snaps to other
  geometry.) Details in [controls.md](controls.md).
- Press **Home** (or **Frame all**) again after moving if the block leaves view.

## 5. Create a material and assign it to region 1

This is the heart of the exercise, and it only works because the example mesh
already has a face material region. Primitive boxes do not (see section 9).

1. Open the Material instrument: **View > Material instrument** (in Focus
   flow it is hidden until you do this).
2. Scroll to the **New material** field, type a name (for example
   `First Paint`), and press **Create PBR source**. The new PBR material joins
   the project.
3. In the **Material source** combo at the top of the instrument, make sure
   your new material is the selected source — pick it from the list if it is
   not already showing.
4. Keep **Region ID** at **1**. The example marks all of its faces with region
   1, and the Region ID must be non-zero to assign.
5. Keep the block as the **only selected object** — direct assignment requires
   exactly one selected mesh object. The instrument shows the selected mesh
   asset and notes the assignment "affects every instance" of that mesh.
6. Press **Assign selected material**. The instrument now reports
   **"Current: First Paint"** (your name) for region 1.

If the button is disabled, the usual causes are: no material selected in
**Material source**, **Region ID** set to 0, or more/fewer than one object
selected. The exact failure messages and their meanings are cataloged in
[troubleshooting.md](troubleshooting.md).

## 6. Save the project

- Press **Ctrl+S** (File > Save). For a project that has never been saved this
  opens the save dialog — or use **File > Save As...** — and you choose the
  location and file name, e.g. `Documents\Cartographer\first-session.carto`.
- Saving writes the `.carto` project document. Once a project path is bound,
  Cartographer keeps a sibling **`<name>.carto.journal`** recovery journal next
  to it and appends verified snapshots as you make further changes. There is
  **no autosave** and no previous-version backup in this build; the journal is
  the only safety net, and recovering from it is explicit: **File > Recover
  latest journal snapshot** (if no journal snapshot is pending for the project,
  the command says so in a dialog instead of recovering anything).
- The **"Unsaved changes"** marker clears once the save lands. Exporting later
  requires this clean state (section 8).

## 7. Exit — and what the prompt really offers

**File > Exit** (or closing the window). If the project has unsaved changes a
modal titled **"Discard Unsaved Changes"** appears and asks *"Discard them and
continue?"* with exactly two buttons:

- **Discard** — closes and loses the unsaved edits.
- **Keep Editing** — returns to the project so you can save.

There is **no Save button in this prompt** — if you want to keep
your work, choose **Keep Editing**, then Ctrl+S, then exit again. If the
project is already saved and clean, the app closes without prompting. This
prompt is also honest about loss: alpha recovery comes only from the journal
snapshot route above, not from an automatic backup.

## 8. Reopen, verify, and export OBJ

1. Relaunch and **File > Open** your saved `.carto`. Opening (like New) also
   asks you to discard if a dirty project is open.
2. Click the block (or its row in **Scene**) and press **Home** to reframe —
   **selection is not persisted** in the project document, so nothing is
   selected after a reopen and you must select again before assigning or
   moving anything.
3. Open the Material instrument and confirm **Region ID 1** still reports
   **"Current: First Paint"**. That confirms the assignment survived the
   save/reopen round trip.
4. Export: **File > Export OBJ** (**Ctrl+Alt+O**). The command is enabled only
   when the project is **saved and unmodified** — the menu literally notes
   *"Export requires a saved, unmodified .carto source"* — so save first if you
   edited after reopening.
5. Know what the OBJ contains: positions and triangulated faces, and, when the
   mesh carries them, active render UVs, custom corner normals, and the
   Cartographer face **material-region IDs** (written as Cartographer metadata
   comments). First Block has no UVs or normals, so its export is positions,
   faces, and region IDs only. Material **assets are always omitted** — the
   OBJ has no material library and no textures. Your `First Paint` definition
   lives only in the `.carto`; the exported OBJ is geometry plus region IDs
   for interchange. Format details and the other export routes (glTF, PLY,
   STL) are in [import-export.md](import-export.md).

## 9. Shortcut alternative: Ctrl+K > Create Box (and its limitation)

You can create geometry without importing a file:

1. Press **Ctrl+K** to open the command palette (the header also shows a
   **"Command palette  Ctrl+K"** button).
2. Choose **Create Box**. A 2 × 2 × 2 m box appears in the project.

**Warning — do not use this path for the material exercise.** A primitive box
is built from raw geometry (`geometry::make_box`) and carries **no face
material-region layer**. With no region IDs on its faces there is nothing for
the Material instrument to bind to at any Region ID, so **Assign selected
material** cannot complete the exercise. The supplied
`examples/first-block/first-block.obj` is required for that part — import it
as in section 3. The same limitation applies to **Create Plane** and to empty
projects generally: no materials and no face material regions exist until you
create or import them.

## 10. Where things live

| Item | Location |
| --- | --- |
| Project document | The `.carto` file you chose at save time |
| Recovery journal | `<project path>.carto.journal`, next to the project |
| Selection, view state | Session only — never written to the `.carto` |
| Material definitions | Inside the `.carto` project document |
| Exported OBJ | Wherever you point the export dialog; no `.mtl` is written |

## Troubleshooting

Quick answers below; the full symptom → fix catalog, exact error text, and
recovery procedure live in [troubleshooting.md](troubleshooting.md).

- **"I can't find the Material/Scene panel."** You are in Focus flow, which
  hides instruments. Use **View > Material instrument** (or **Scene
  instrument**), switch to Author (**Ctrl+Shift+2**), or **View > Reset
  workspace layout**.
- **Export OBJ is greyed out.** Export requires a saved, unmodified `.carto`:
  save (Ctrl+S) first. The menu shows the reason as disabled text.
- **Assign selected material is greyed out.** No material selected under
  **Material source**, Region ID is 0, or you do not have exactly one mesh
  object selected.
- **Exit asked me to discard work I wanted.** The prompt has no Save option;
  choose **Keep Editing**, save, then exit. Plan to save before closing.
- **A crash lost my last edits.** There is no autosave. Reopen the project and
  try **File > Recover latest journal snapshot**; it replays only journaled,
  valid snapshots and stops at the last coherent revision rather than silently
  repairing anything.
- **Frame all did nothing.** A status message appears if the scene cannot fit
  the supported view range; zoom out with the wheel and retry.

## Related documents

- [Example asset and rebuild notes](../../examples/first-block/README.md) —
  what First Block contains and how to regenerate it
- [Controls and shortcuts](controls.md) — the full code-checked binding table
  (navigation, handles, flows, palettes)
- [Import/export guide](import-export.md) — what import admits and each export
  route emits
- [Troubleshooting and recovery](troubleshooting.md) — symptom → fix, exact
  error text, and the safe recovery procedure
- [In-app help copy](in-app-help.md) — proposed onboarding/help text keyed by
  topic ID; **nothing proposed there is shipped**, and
  [the Sol integration handoff](sol-integration-handoff.md) records what wiring
  is still owed
- [Evidence checklist](evidence.md) and
  [headless example validation](example-validation.md) — what is proven, and
  how far
- [Alpha blockers](alpha-blockers.md) — known first-use gaps (no in-app Help,
  pending native acceptance)
- [Controls and UI architecture](../architecture/ui.md) and
  [viewport interaction boundary](../architecture/standalone-dcc-viewport-interaction.md) —
  the input and presentation contract behind the labels above
- [Import/export providers](../architecture/providers.md) and
  [project package boundary](../architecture/project-package.md) — what
  import and export actually admit and emit
- [Project schema v35](../format/cartographer-project-v35.md) and
  [journal boundary](../architecture/journal.md) — the saved document and
  recovery format (schemas v1–v35 are readable; the schema-34 example upgrades
  on save)
