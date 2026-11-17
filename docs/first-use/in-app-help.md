# Cartographer — in-app help copy (alpha first-use)

> Historical handoff from 2026-10-28, recovered during worktree consolidation. Workstation paths use placeholders. UI labels, source line references and unimplemented proposals describe that dated candidate. They are not verification of the current application. See [the current texturing verification](../verification/texturing-workflow.md) and [integration ledger](../verification/worktree-integration-20261116.md).


Ready-to-integrate help copy for the desktop shell. Every section carries a
stable **help topic ID** so the shell can route Help entries, the welcome
card, and contextual error hints to the same text.

Conventions used throughout this file:

- **[existing]** — label, shortcut, or behavior verified in the current shell.
- **[proposed]** — UI or route that does not exist yet and needs wiring
  (first-launch card, Help menu, F1). Nothing proposed here is shipped.

This file is copy only. It contains no implementation guidance beyond what the
shell needs to place the text.

---

## Integration contract

- Each topic below is keyed by its `help.*` ID. The shell resolves an ID to
  the topic's title and body verbatim.
- **[proposed]** routes:
  - A **Help > First Steps** menu item opens the welcome/quick-start card.
  - **F1** opens the same help surface, focused on the panel or instrument
    that was active when F1 was pressed (context-sensitive fallback: the
    quick start card).
- One dismissal flag controls the first-launch card: after the user dismisses
  it once, it never appears on launch again. **Help > First Steps** and
  **F1** still open it on demand.

---

## help.welcome.first-launch — First-launch welcome card [proposed]

Nonmodal card shown once, docked to the side of the work surface so the
Viewport stays fully visible. It never takes keyboard focus on open; the user
keeps working and can ignore it.

**Title:** Welcome to Cartographer

**Body:**

> Cartographer is a governed spatial authoring workbench. Your work lives in a
> saved `.carto` project, and every change is recorded as a revision you can
> inspect and undo.
>
> **Three steps to your first object:**
> 1. Press **Ctrl+K**, choose **Create Box**.
> 2. Or import one: **File > Import Mesh…** (**Ctrl+I**) accepts OBJ, PLY, or
>    STL.
> 3. Save with **Ctrl+S**. Saving binds the project to disk and starts the
>    journal that powers recovery.
>
> On your first launch, Cartographer opens in the **Focus** workbench: every
> instrument is hidden so only the work surface stays visible — **Ctrl+K**
> still reaches every command. Later launches restore your last workbench
> layout.

**Buttons:**

- **Get started** — closes the card and opens the quick start
  (`help.quick-start`).
- **Don't show again** — primary dismissal. Sets the never-show-on-launch
  flag. The card does not reappear on future launches; **Help > First
  Steps** or **F1** [proposed] reopen it at any time.

**Placement and behavior requirements:** nonmodal; no focus steal; must not
cover the Viewport; dismissed state persists across launches.

---

## help.quick-start — Three-step quick start

**Title:** Quick start: box, mesh, save

**Body:**

> **1. Create a box.** Press **Ctrl+K** [existing] to open the Command
> Palette, type `box`, and run **Create Box**. A new box appears as a project
> object with no material.
>
> **2. Or import a mesh.** Use **File > Import Mesh…** (**Ctrl+I**)
> [existing]. The file dialog accepts OBJ, PLY, and STL. Imported meshes
> enter through the same governed history as everything else.
>
> **3. Save.** Press **Ctrl+S** (**File > Save**) [existing], or
> **File > Save As…** on a new project. See `help.project.first-save` for why
> saving matters here.
>
> Working on the practice asset instead? Open
> `examples/first-block/first-block.carto` and follow `help.first-block`.

---

## help.first-block — First Block practice hint

**Title:** First Block: ready for its first material

**Body:**

> The bundled **First Block** example (`examples/first-block/first-block.carto`)
> is a small original block: 1 object, 8 vertices, 6 quad faces. Every face
> declares material region **1**, and the project ships with **no material** —
> so it is the fastest way to try material authoring end to end.
>
> 1. **File > Open** and choose `first-block.carto`. The file is schema 34;
>    this build reads it and upgrades it to the current format on save.
> 2. In the **Material** instrument [existing], type a name and choose
>    **Create PBR source**.
> 3. With the block selected, keep **Region ID** at **1** and choose
>    **Assign selected material**.
>
> Prefer the import route? Use **File > Import Mesh…** on
> `examples/first-block/first-block.obj` instead.

---

## help.project.first-save — Your first save

**Title:** What saving does

**Body:**

> **File > Save** (**Ctrl+S**) binds the project to a `.carto` file on disk.
> Until then the project is "Not saved to disk" and exists only for this
> session.
>
> Saving matters because several features are gated on a saved project:
>
> - **Recovery.** A saved project keeps a recovery journal. If something goes
>   wrong, **File > Recover latest journal snapshot** [existing] restores the
>   newest journaled state. There is no automatic recovery and no automatic
>   backup of previous versions — recovery is always an explicit choice you
>   make from the File menu.
> - **Export.** Export commands (glTF, GLB, OBJ, PLY, STL) are enabled only
>   while the project is saved **and** unmodified. Make a change, save again,
>   then export. See `help.recovery-export`.
> - **Texture sources.** **Import Texture Source…** in the Material
>   instrument requires a saved project.

---

## help.workspace — Workspace

**Title:** Workspace

**Body:**

> The **Workspace** surface is where you author geometry. It is built from a
> Viewport plus dockable instruments — Scene, Assets, Inspector, Material,
> History, and more.
>
> - Compose a task-specific arrangement with **Compose** in the header
>   [existing]. Flows include **Focus**, **Author**, **Inspect**, and
>   **Draft**; shortcuts are **Ctrl+Shift+1** through **Ctrl+Shift+4**, and
>   **Ctrl+Shift+R** resumes your learned flow [existing].
> - **Focus** hides every instrument so only the work surface stays visible —
>   it is the first-launch default. All commands, creation included, remain
>   available through **Ctrl+K**.
> - The left rail switches between **Workspace**, **Project**, **Market**,
>   **Diagnostics**, and **Settings** surfaces [existing].
> - The status strip at the bottom shows the project name, revision, object
>   count, and save state at a glance [existing].

---

## help.viewport — Viewport

**Title:** Viewport and work surface

**Body:**

> The **Viewport** shows the current scene and receives your pointer input.
>
> - **Pan:** drag with the middle or right mouse button, drag the left button
>   on empty background, or hold **Space** or **Alt** while dragging; the
>   arrow keys nudge the view [existing].
> - **Zoom:** scroll the mouse wheel over the Viewport; zoom centers on the
>   cursor position [existing].
> - **Select** objects by clicking them; the active selection mode
>   (Object / Vertex / Edge / Face) is set in the toolbar or with **1 / 2 / 3 /
>   4** [existing].
> - **Undo / Redo:** **Ctrl+Z** / **Ctrl+Y** [existing].
> - If the status area shows **Render unavailable**, the viewport snapshot
>   failed — see `help.error.viewport-unavailable`.

---

## help.scene — Scene

**Title:** Scene instrument

**Body:**

> **Scene** lists every object on the work surface [existing]. Click a name to
> select it; right-click for **Ask Cartographer about this object**. An empty
> project shows **No project objects** — use **Ctrl+K > Create Box** or
> **File > Import Mesh…** to add the first one.

---

## help.inspector — Inspector

**Title:** Inspector instrument

**Body:**

> **Inspector** shows the selected object and its mesh elements [existing].
> With one object selected you can review its presentation and open the
> **Transform** or **Fidelity** instruments from here. In Vertex mode with a
> single vertex selected, position fields and **Apply Vertex** edit that
> vertex directly [existing].
>
> Nothing selected? The Inspector prompts you to **Select an object or mesh
> element** — click something in the Viewport or in Scene.

---

## help.assets — Assets

**Title:** Assets instrument

**Body:**

> **Assets** is where project asset content is organized and browsed
> [existing]. Imported meshes and authored collections appear here as they
> are added to the project. Assets are project-owned: they persist inside the
> saved `.carto`, not in loose sidecar files.

---

## help.material — Material

**Title:** Material instrument

**Body:**

> **Material** authors project-owned PBR material source, persisted in the
> `.carto` [existing].
>
> - Type a name and choose **Create PBR source** to add your first material.
>   New projects and new boxes have **no materials** and no face
>   material-region layer until you create one.
> - **Mesh-region assignment** binds a material to a numeric **Region ID** on
>   a mesh. **Assign selected material** applies it to every instance of that
>   mesh; **Clear region assignment** removes it [existing].
> - Texture sources import with **Import Texture Source…** (saved projects
>   only); pixels are inspected, not decoded or rendered [existing].
> - Structural graph checks validate material graph wiring; they do not
>   compile GPU shaders or change live viewport shading [existing].

---

## help.history-operations — History and Operations

**Title:** History, Operations, and undo

**Body:**

> **History** (the operation ledger) records every committed action as a
> receipt: action name, revision before and after, and whether the document
> changed [existing]. Select an entry to inspect its receipt.
>
> - **Edit > Undo** (**Ctrl+Z**) and **Edit > Redo** (**Ctrl+Y**) walk the
>   revision history [existing].
> - The bottom ledger tabs also expose **Graph**, **Assets**, **Timeline**,
>   **Problems**, and **Console** panels [existing].
> - Receipts are session records, not a second project history — they inform,
>   they do not fork your work.

---

## help.project — Project

**Title:** Project surface

**Body:**

> The **Project** surface is the durable boundary of the current workspace
> [existing]. It shows the project name, location, save status, revision,
> object count, and undo/redo depth.
>
> - **Save Project** / **Save As** / **Open Another** buttons live here
>   [existing].
> - **Not saved to disk** means the session has no recovery journal yet — save
>   to establish one (see `help.project.first-save`).
> - Switch surfaces any time from the left rail.

---

## help.settings-diagnostics — Settings and Diagnostics

**Title:** Settings and Diagnostics

**Body:**

> **Settings** holds presentation preferences only: theme (Dark / Light / High
> contrast), density (Compact / Standard / Touch), and the operator boundary
> (Person-first / AI-first, with auto-approve of safe native studies)
> [existing]. These are local UI state, not project content: the
> authoritative copy of your project — objects, assets, materials,
> revisions — lives in the saved `.carto`/session document, and nothing here
> changes it.
> The workflow shortcut list is repeated here for reference [existing].
>
> **Diagnostics** reports observed state only: native shell availability,
> viewport snapshot readiness, persistence state, surface fidelity activity,
> and current problems [existing]. It never invents capability — if a check
> was not run, it is not shown as passed. Use it first when something looks
> wrong; then check the **Problems** tab in the bottom ledger.

---

## help.layout — Docking, focus, and reset

**Title:** Arrange the work surface

**Body:**

> - **Docking:** every panel is an ordinary dockable window — drag its tab to
>   move, snap, tab, float, or tear it out to its own OS window [existing].
> - **Hide and reopen:** close a panel's tab to hide it; reopen from
>   **View** [existing].
> - **Focus:** **F11** (or **View > Viewport focus / restore**) shows the
>   Viewport alone; press **F11** or **Escape** to restore the layout
>   [existing].
> - **Reset:** **View > Reset workspace layout** restores the default
>   arrangement [existing].
> - **Flows:** **Compose** in the header re-arranges instruments for a task;
>   manual instrument toggles mark the layout **Custom** [existing].

---

## help.recovery-export — Recovery and export

**Title:** Recovery and export rules

**Body:**

> **Recovery** [existing]: **File > Recover latest journal snapshot** restores
> the newest journaled state of the saved project. Recovery is explicit —
> there is no autosave and no automatic previous-version backup. Unsaved
> session changes are not journaled, so save early and save after meaningful
> steps.
>
> **Export** [existing]: **File > Export glTF / GLB / OBJ / PLY / STL** are
> enabled only when the project is saved **and** unmodified. If export is
> disabled, save first (**Ctrl+S**); the disabled menu items say exactly why:
> *"Export requires a saved, unmodified .carto source"*. If you invoke an
> export route on an unsaved or modified project, the shell reports it word
> for word: *"Save the .carto project before exporting a glTF asset."* or
> *"Save the .carto project before exporting a mesh asset."*
>
> Import note: **Import Mesh…** accepts **OBJ, PLY, and STL** meshes. glTF is
> an export format in this build, not an import target.

---

## Contextual error help

Short hints shown next to the error that triggered them. IDs are stable;
wire each to the matching diagnostic surface, dialog, or disabled control.

### help.error.not-saved — "Not saved to disk" / save-state warning

> This project has no file bound yet. Choose **File > Save** (**Ctrl+S**) or
> **File > Save As…**. Saving starts the recovery journal and unlocks export
> and texture import. See `help.project.first-save`.

### help.error.export-unavailable — "Save the .carto project before exporting a glTF asset." / "Save the .carto project before exporting a mesh asset."

> Export needs a saved, unmodified project. Save now, then run the export
> again. If you just edited something, that change must be saved first — that
> is the rule, not a fault. The same rule disables the export items in the
> File menu until the project is saved and clean, with the in-menu note
> *"Export requires a saved, unmodified .carto source"*. See
> `help.recovery-export`.

### help.error.no-materials — "No material sources in this project."

> Expected for new projects, new boxes, and the First Block example. Create
> one: **Material** instrument → type a name → **Create PBR source**. Then
> select a mesh object and choose **Assign selected material** for a Region
> ID. See `help.material`.

### help.error.no-region-assignment — "No material is assigned to this region."

> The Region ID on this mesh has no material bound. Select a material in the
> Material instrument and choose **Assign selected material**, or pick a
> different **Region ID**. Region 1 is a good default — First Block's faces
> all declare region 1.

### help.error.viewport-unavailable — "Render unavailable" / viewport snapshot error

> The viewport snapshot could not be produced; authoring history and panels
> still work. Check the message on the **Diagnostics** surface and the
> **Problems** tab in the bottom ledger for the specific failure [existing].

### help.error.problems-tab — Reading the Problems tab

> **Problems** lists active diagnostics with an error code and message
> [existing]. **Diagnostics** adds capability state — what is actually
> available right now, not what should be. Treat a green Diagnostics state as
> "observed working", never as a guarantee of untested behavior.

### help.error.import-mesh-failed — Mesh import rejected

> **Import Mesh…** accepts OBJ, PLY, and STL. Other formats (including glTF)
> are not import targets in this build. If a supported file still fails, the
> error text names the validation that rejected it — fix the file and import
> again.

### help.error.recover-no-snapshot — Recovery found no snapshot

> **Recover latest journal snapshot** only works for a saved project with
> journal history. If the project was never saved, there is nothing to
> recover — save early so a journal exists. See `help.recovery-export`.

---

## Copy rules for integrators

- Do not paraphrase these bodies into claims about unverified behavior. In
  particular, this copy must never be surfaced as promising: 3D orbit camera,
  built-in/shipped materials, autosave or automatic backups, or glTF import.
- Menu and button labels quoted here match the current shell; keep them in
  sync if labels change.
- The welcome card and all **[proposed]** routes are opt-in surfaces: nothing
  here may block startup, steal focus, or reappear after dismissal.
