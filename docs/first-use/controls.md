# First-use controls reference

> Historical handoff from 2026-10-28, recovered during worktree consolidation. Workstation paths use placeholders. UI labels, source line references and unimplemented proposals describe that dated candidate. They are not verification of the current application. See [the current texturing verification](../verification/texturing-workflow.md) and [integration ledger](../verification/worktree-integration-20261116.md).


- **Source revision:** `fae4f7d95259ba38e79edc94ae12275df4161cd4`
- **Worktree:** `<checkout>/.worktrees/first-use-20261028`
- **Date:** 2026-10-28
- **Scope:** the Vulkan desktop workbench (`apps/desktop`). The D3D12 shell is a
  separate target and is not covered here.
- **Status:** every binding below is code-derived from the pinned revision. No
  candidate build was launched; nothing here is a runtime/native acceptance
  result. See [evidence.md](evidence.md) for the evidence policy.

## Read this first: the viewport is a 2D plan, not a 3D view

The workbench viewport is a **2D orthographic X/Z plan view**. The camera
projects world `x`/`z` to screen (`apps/desktop/main.cpp:7169-7181`,
`work_surface_camera.world_to_screen({world.x, world.z})`). There is **no 3D
orbit, no camera rotation, and no Y-axis viewport control** in this baseline —
do not expect Alt+MMB tumble or similar. Height exists in the data, but the
workbench presents the top-down plan.

There is also **no Help menu and no F1 route** in this baseline. This document
is the onboarding substitute.

## File / Edit

| Action | Shortcut | Notes |
|--------|----------|-------|
| New / Open / Save As / Exit | — | Menu only (no Ctrl+N / Ctrl+O; do not assume them). `apps/desktop/main.cpp:6242-6268`, `6325` |
| Import Mesh | `Ctrl+I` | File menu and key route. `main.cpp:6260`, `6177-6179` |
| Save | `Ctrl+S` | `main.cpp:6264`, `libs/ui/src/ui.cpp:2473` |
| Recover latest journal snapshot | — | File menu only. There is **no autosave or previous-version backup**; recovery is explicit. `main.cpp:6270` |
| Undo / Redo | `Ctrl+Z` / `Ctrl+Y` | `main.cpp:6329-6330` |
| Duplicate Selected | `Ctrl+D` | Only in object mode with exactly one object selected. `main.cpp:6332-6337`, `6180-6182` |
| Repeat Last Tool | `Ctrl+R` | Dispatches `RepeatLastToolAction`. `libs/ui/src/ui.cpp:2476`, `11689-11693` |

## Command palette

| Action | Shortcut | Notes |
|--------|----------|-------|
| Toggle command palette | `Ctrl+K` | `libs/ui/src/ui.cpp:2477`; palette window at `main.cpp:6462-6521` |

The palette filters as you type (matches command label or id) and runs the
selected command. It is the only first-run route to creation tools: the default
Focus workbench flow hides the creation instruments, so **Ctrl+K > Create Box**
is how you add your first box (`create.box` → `CreateBoxAction`, a 2×2×2 m box;
`main.cpp:6514`, `libs/ui/src/ui.cpp:12107`). The palette also exposes Create
Plane, selection-mode switches, person-first/AI-first operator mode, Reset
Workspace, and market refresh (`ui.cpp:12096-12148`).

## Selection modes and viewport mouse

| Action | Input | Notes |
|--------|-------|-------|
| Object / Vertex / Edge / Face mode | `1` / `2` / `3` / `4` | `libs/ui/src/ui.cpp:2484-2487`; also a Mode combo in the toolbar (`main.cpp:6427-6436`) |
| Select | Left-click | Replace selection. |
| Add to selection | `Shift` + left-click | `libs/ui/src/ui.cpp:2590` |
| Toggle selection | `Ctrl` + left-click | `ui.cpp:2589`; `Alt` or `Ctrl+Shift` clicks intentionally select nothing (`ui.cpp:2588`) |

## Viewport navigation (2D plan pan/zoom)

| Action | Input | Notes |
|--------|-------|-------|
| Frame all | `Home` or the **Frame all** button | Fits scene bounds; warns if the scene cannot fit the supported view range. `main.cpp:6865-6877`, `8333-8336` |
| Pan view | Arrow keys (repeat) | ~16% of viewport per press, diagonals scaled by 1/√2. `main.cpp:7149-7168`, `libs/ui/src/ui.cpp:2045-2066` |
| Pan view | Left-drag on empty background | `libs/ui/src/ui.cpp:2018-2026` (`work_surface_pointer_requests_pan`) |
| Pan view | Right-drag or middle-drag | Anywhere over the viewport, including over geometry. `main.cpp:20260-20278`, `ui.cpp:2005-2019` |
| Pan view | `Space`+left-drag, `Alt`+left-drag, or the **Pan view** toggle + left-drag | `main.cpp:20290-20293`, `6883-6894` |
| Zoom | Mouse wheel | Zooms at the cursor position. `main.cpp:7140-7148` |
| Nudge view | `<` `^` `v` `>` buttons | On-screen pan buttons beside the viewport. `main.cpp:6907-6917` |

Holding `Space` makes the primary button pan instead of select for that drag
(`main.cpp:6946-6950`, `20257-20293`).

## Moving objects: MOVE X / MOVE Z handles

With exactly one unlocked object selected in a workspace that supports scene
translation, the viewport draws labeled **MOVE X** (right) and **MOVE Z**
(up-screen) arrow handles at the object center (`main.cpp:8198-8234`). Drag a
handle to translate along that world axis:

- Hold **`Ctrl`** while dragging for **0.25 m grid snap**.
- Hold **`Shift`** while dragging for **geometry snap** (vertex / edge midpoint /
  face center of other objects, within 0.25 m).
- Otherwise the drag is a free axis-constrained preview; release to commit.

`main.cpp:3771-3803`; the exact-position hint is `main.cpp:10949-10951`
("Drag MOVE X / MOVE Z in the view, or set an exact position."), and the
viewport pan/move hint text is at `main.cpp:6920-6922`.

## Focus mode, workbench flows, and panel toggles

| Action | Shortcut | Notes |
|--------|----------|-------|
| Viewport focus / restore | `F11` or `Ctrl+Shift+V` | Hides the workbench chrome for a solo viewport. `main.cpp:6126-6129`, `6377-6380` |
| Exit focus, restore workbench | `Escape` | `main.cpp:6130-6132` |
| Focus / Author / Inspect / Draft flow | `Ctrl+Shift+1` / `2` / `3` / `4` | Preset instrument layouts. `main.cpp:6156-6163`, `2634-2671` |
| Re-apply remembered flow | `Ctrl+Shift+R` | Re-runs the most recently used non-custom flow. `main.cpp:6164-6167` |
| Toggle operation ledger | `Ctrl+Shift+L` | `main.cpp:6168-6170` |
| Toggle inspector | `Ctrl+Shift+I` | `main.cpp:6171-6173` |
| Toggle transform instrument | `Ctrl+Shift+T` | `main.cpp:6174-6176` |

Flow layouts (`main.cpp:2634-2671`): **Focus** shows no instruments (viewport
only — first-run default); **Author** adds Scene, Tools, Inspector; **Inspect**
adds Scene, Inspector, Transform, Measure; **Draft** adds Inspector and the
draft instrument. Any manual layout change marks the flow **Custom**.

## Prepare and export

| Action | Shortcut | Notes |
|--------|----------|-------|
| Prepare scene / selection | `Ctrl+Alt+B` / `Ctrl+Alt+Shift+B` | `main.cpp:6183-6184`, `6294-6302` |
| Export glTF | `Ctrl+Alt+E` | GLB variant is menu-only. `main.cpp:6191-6192`, `6307-6311` |
| Export OBJ / PLY / STL | `Ctrl+Alt+O` / `Ctrl+Alt+P` / `Ctrl+Alt+S` | `main.cpp:6185-6190`, `6313-6321` |

All prepare/export entries are gated: they require a **saved, unmodified
.carto** project; unsaved edits block them ("Export requires a saved, unmodified
.carto source", `main.cpp:6290`, `6322-6324`). Save first with `Ctrl+S`.

## Shortcuts and text input

Global shortcut routing returns early while a text field wants keyboard input
(`main.cpp:6125`, `io.WantTextInput`), and viewport camera hotkeys additionally
require no active text input and no captured keyboard
(`libs/ui/src/ui.cpp:2035-2043`). In practice: while typing in a text box —
including the command palette's search field — `1-4`, `Ctrl+S`, `Ctrl+Z`, arrow
pan, and `Home` are ignored, and typed characters go to the field. `Escape` is
checked before that guard in two places: it cancels an active instrument
placement unconditionally (`main.cpp:6112-6117`), and it cancels a pending
loop-cut seed pick when no text field is active — that check keeps its own
`!WantTextInput` condition even though it runs ahead of the guard
(`main.cpp:6118-6124`). `Escape` otherwise closes the top overlay
(command palette and similar) via `Shortcut::close_overlay`
(`libs/ui/src/ui.cpp:2488`), and exits focus mode first when it is active
(`main.cpp:6130-6132`, `6155`).

## Docking, panel tabs, and detachable windows

- **Docking** uses the pinned Dear ImGui docking build
  (`apps/desktop/carto_studio_dock.hpp:1-21`). Panels dock and resize
  independently; closing a tab hides the pane, and hidden panes reopen from
  **View** (`main.cpp:6388`, `20328-20336`).
- **Panel tabs:** an instrument docked as a tab shows the hint "Drag this tab to
  split, snap, or detach" plus a **MOVE / FLOAT** button (`main.cpp:5851-5857`).
  A floated instrument shows a drag grip labeled **DRAG TO MOVE · SNAP** (or
  **DRAG** when narrow), which moves the panel and previews a snap rectangle
  against other panels and the work-surface bounds
  (`libs/ui/src/ui.cpp:1859-1863`; preview at `main.cpp:5826-5837`).
- **Detachable OS windows:** panel/viewport tear-out into native HWNDs is
  implemented via ImGui platform viewports (`main.cpp:20834-20835`,
  `carto_studio_dock.hpp:229-230`). If the Win32/Vulkan backends do not enable
  platform windows, tear-out is disabled with an explicit status message rather
  than failing (`main.cpp:20844-20850`). Detached windows that end up offscreen
  (e.g. a disconnected monitor) are clamped back onto a live monitor
  (`carto_studio_dock.hpp:116-220`). This behavior is **code-checked, not
  candidate runtime-accepted** — native picker/device acceptance is still open
  (`docs/roadmap.md:198-199`).
- **View > Reset workspace layout** (`main.cpp:6381-6387`) restores the default
  workbench, re-enables the Scene/Assets/Inspector instruments, and rebuilds the
  initial dock layout (Viewport center, Scene left, Inspector right,
  Assets+History bottom; `carto_studio_dock.hpp:280-294`).

## Explicitly not present in this baseline

- No **F1** / Help menu / onboarding content.
- No **Ctrl+N** / **Ctrl+O** (New and Open are menu-only).
- No 3D orbit / camera-rotate controls — the viewport is the 2D X/Z plan.
- No autosave; recovery is explicit via **File > Recover latest journal snapshot**.

## Verification note

All bindings verified by reading source at `fae4f7d`:

- Global shortcut routing and workbench-flow keys: `apps/desktop/main.cpp:6110-6194` (F11/`Ctrl+Shift+V` focus toggle `6126-6132`; flows `6156-6163`; ledger/inspector/transform `6168-6176`; import/duplicate `6177-6182`; prepare/export `6183-6193`).
- Menu-declared accelerators: `main.cpp:6260-6337` (File/Edit), `6377-6387` (View focus/reset).
- Native key → shortcut contract: `libs/ui/src/ui.cpp:2459-2493`; handling at `11689-11716`.
- Viewport pan/zoom/frame and MOVE X/Z handles: `main.cpp:6865-6946`, `7140-7168`, `8198-8234`, `20257-20298`; helpers `libs/ui/src/ui.cpp:2005-2066`, `2574-2592`.
- Command palette contents and activation: `libs/ui/src/ui.cpp:12094-12148`; `apps/desktop/main.cpp:6462-6521`.
- Docking/tear-out: `apps/desktop/carto_studio_dock.hpp:222-294`; `main.cpp:20828-20850`.
- Snap-drag grip label cross-checked against `tests/desktop_workbench_input_tests.cpp:511`.

Not verified at runtime: no candidate GUI was launched in this lane, so none of
the above has been exercised against a build of `fae4f7d`.
