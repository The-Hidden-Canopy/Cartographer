# First-Use In-App Help — Integration Handoff to Sol

> Historical handoff from 2026-10-28, recovered during worktree consolidation. Workstation paths use placeholders. UI labels, source line references and unimplemented proposals describe that dated candidate. They are not verification of the current application. See [the current texturing verification](../verification/texturing-workflow.md) and [integration ledger](../verification/worktree-integration-20261116.md).


**Status:** proposal only. Nothing in this document is wired today.
**Baseline:** worktree commit `fae4f7d` ("Import static GLTF scenes through governed
product-bound history"). All file/line references are to that commit and will drift.
**Scope of this lane:** documentation only. This lane wrote no UI code, no library
code, and no tests.

---

## 1. What this handoff is for

The first-use package in this worktree contains the help *content* under
`docs/first-use/` — the in-app help copy spec in `docs/first-use/in-app-help.md`
and the companion guides (`controls.md`, `import-export.md`,
`troubleshooting.md`, `startup-guide.md`) — plus the validated example project
in `examples/first-block/`. All of these files exist in the baseline worktree;
none is pending. This document tells integration (Sol) exactly where that
content should surface in the desktop shell, what behavior each surface must
have, which existing code owns each integration point, and which focused tests
must pass before the wiring counts as done.

Nothing here has been implemented. There is no Help menu, no F1 handler, no help
panel, no panel-level `?` links, and no first-run presentation in the baseline. Do
not describe any of it to users as existing.

Content contracts the UI must honor (authored by the content lanes, restated here
so integration tests assert the right things):

- `docs/first-use/in-app-help.md` — the topic library for all help copy: the
  welcome card, quick start, per-surface topics, and contextual error hints.
  As of this handoff it defines 15 user-facing topics plus 8 contextual
  error topics; that count is **not fixed** and must not be asserted in
  wiring or tests. The Help menu in 3.1 proposes exactly four *entries*
  (First Steps, Controls, Import / Export, Troubleshooting). Each entry is
  assembled from one or more topics in this library together with the
  companion guides listed above — e.g. the Controls entry's shortcut table
  lives in `controls.md`, the Troubleshooting entry draws on
  `troubleshooting.md` plus the `help.error.*` hints.
- `examples/first-block/` — `first-block.carto`, `first-block.obj`,
  `generate_example.py`, `manifest.json`, `README.md`, `PROVENANCE.md`. The
  project was generated and validated by the pre-existing Cartographer CLI 0.1.0
  at schema 34 (`manifest.json` records `generator_cli_version` 0.1.0 and
  `CARTOGRAPHER_PROJECT 34`). It has 1 object, 8 vertices, 6 quads / 12
  triangles, region 1 on all faces, and **no initial material**. Current source
  schema is 35 (`Project::kSchemaVersion`, `libs/project/include/carto/project/project.hpp:326`)
  and the reader accepts v1–v35 (`libs/project/src/project.cpp:5816-5823`), so the
  example loads as a backward-compatible document.
- Honest limitations help must state, not paper over: empty projects and new boxes
  have no materials and no face material-region layer; the Focus workbench flow
  (the first-launch default, `apps/desktop/main.cpp:1802`) hides every
  instrument, so a first-run user reaches creation via
  Ctrl+K → "Create Box" (`create.box` in `UiController::command_views`,
  `libs/ui/src/ui.cpp:12107`); native export requires a saved, unmodified `.carto`;
  there is no autosave and no previous-version backup — recovery is the explicit
  **File > Recover latest journal snapshot** action
  (`apps/desktop/main.cpp:6270-6272`).

---

## 2. Ownership statement (why this lane touched no code)

Active integration owns:

- `apps/desktop/main.cpp` (20,898 lines — menu bar, shortcut routing, command
  palette, workbench composition, preference load/save at startup);
- shared UI state in `libs/ui/` (`include/carto/ui/ui.hpp`, `src/ui.cpp` —
  `UiPreferences`, `WorkbenchPreferences`, `Shortcut`/`NativeKey` translation,
  `UiController`);
- shared application state in `libs/application/`;
- test registration in `tests/CMakeLists.txt`.

Every integration point below lands in one of those files. This lane therefore
made **no edits to `apps/`, `libs/`, `tests/`, `README.md`, CMake, or any other
lane's files**, and produced this handoff plus the content/docs in its own scope
only. All proposed line numbers are insertion *sites adjacent to* existing code,
not modifications this lane performed.

---

## 3. Proposed integration points and behavior

### 3.1 Help menu (must-have)

There is no Help menu today. `draw_menu()` (`apps/desktop/main.cpp:6239-6418`)
ends with the Window menu and `ImGui::EndMainMenuBar()` at line 6417. Insert a
`Help` menu between `Window` (ends line 6415) and `EndMainMenuBar()`:

- **Help > First Steps** — opens the docked First Steps/Help tab (3.3) on the
  First Steps topic, even after first-launch dismissal (3.4). This is the manual
  reopening path.
- **Help > Controls** — opens the same tab on the Controls topic (shortcut table:
  Ctrl+K command palette, Ctrl+S save, Ctrl+I import mesh, 1–4 component modes,
  F11 viewport focus, Ctrl+Shift+1–4 workbench flows; the underlying tables live
  in `docs/first-use/controls.md` and mirror `workbench_flow_shortcut()` at
  `apps/desktop/main.cpp:1142-1148`).
- **Help > Import / Export** — opens the tab on the Import / Export topic,
  including the saved-unmodified-`.carto` export precondition (the export menu
  items already gate on `can_export`, `apps/desktop/main.cpp:6307-6319`).
- **Help > Troubleshooting** — opens the tab on the Troubleshooting topic:
  no autosave, **File > Recover latest journal snapshot** (line 6270), schema
  version behavior, empty-project "no material" explanation, and where logs and
  preference files live.

Behavior requirements for all four items: nonmodal; never blocks the viewport;
re-entrant (choosing a topic while the tab is open switches topic, not window).

### 3.2 F1 (must-have)

F1 opens the same tab on the First Steps topic. Two wiring sites are required so
the shortcut works in every window state:

- `route_shortcuts()` (`apps/desktop/main.cpp:6110-6194`) — add an
  `ImGui::IsKeyPressed(ImGuiKey_F1, false)` branch. This is the path that works
  when keyboard focus is in a **torn-out OS window**: the Win32 procedure of the
  main window never sees keys typed there, so shortcuts must route from ImGui
  input (existing comment at lines 6133-6134; the `invoke_native` helper at
  6139-6155 exists for exactly this reason).
- The native translation path for the main window: extend `enum class NativeKey`
  (`libs/ui/include/carto/ui/ui.hpp:174-185`) with `f1` and map it in
  `shortcut_for_native_key()` (`libs/ui/src/ui.cpp:2459-2493`) to a new
  `Shortcut::open_help` case in `enum class Shortcut` (`ui.hpp:156-169`) handled
  in `UiController::handle_shortcut()` (`ui.cpp:11672-11720`). Add the Win32
  key event for F1 next to the existing `invoke_native` calls.

Guard conditions must match the existing conventions in `route_shortcuts`:
`io.WantTextInput` returns early (line 6125), and F1 must not fire while a text
field is capturing input.

### 3.3 Nonmodal docked First Steps/Help tab (must-have)

A dockable ImGui window (not a modal, not a popup, not a blocking overlay):

- Title "First Steps" on first launch; tab host label "Help" once dismissed and
  reopened via the Help menu or F1.
- Default docking position: docked to a side of the dock region (roughly a
  third of its width), or tabbed with another docked pane — never floating
  over the viewport center. Do not assume the Inspector pane is visible as an
  anchor: first launch runs the Focus flow, which shows no instruments
  (3.4, `compose_workbench_flow()` at `apps/desktop/main.cpp:2634-2671`).
  On first show the tab must not cover the viewport's interactive area.
- **No foreground steal and no modal capture.** The tab must not call
  `ImGui::OpenPopup`, must not set `ImGuiWindowFlags_Modal`, must not raise or
  activate the OS window, and must leave `io.WantCaptureKeyboard`/`WantTextInput`
  untouched for the viewport. Opening it must not move keyboard focus out of a
  focused text field. The viewport remains fully usable while the tab is open;
  closing the tab is a normal dock close, equivalent to View-menu panel toggles
  (`apps/desktop/main.cpp:6340-6376`).
- Content is the four Help-menu *entries* named in 3.1 (First Steps,
  Controls, Import / Export, Troubleshooting), each assembled from the topic
  library in `in-app-help.md` plus the companion guides listed in section 1
  (the Controls entry's tables come from `controls.md`), selectable inside
  the tab. The topic count in `in-app-help.md` is not fixed — do not
  hard-code it. All content is offline-rendered text; the only "links" are
  in-app actions (see 3.5).
- Registering it as a first-class panel would require extending
  `enum class Panel` (`ui.hpp:102-112`), which is shared workspace state synced
  into the application layer via `UiController::sync_application_workspace()`
  (`ui.cpp:12084-12092`). **Recommended for alpha:** keep the help tab as a
  shell-local dock window (like the docking layout in `carto_studio_dock.hpp`),
  not a `Panel` — this avoids touching workspace truth and keeps the tab out of
  the `.carto` document and out of workspace prefs.

### 3.4 First-launch-only presentation and persistent dismissal (must-have)

- **First launch** = `workbench.prefs` (or the help flag's own key in it) does
  not exist. `load_workbench_preferences()` already treats `not_found` as
  defaults (`apps/desktop/main.cpp:2429-2446`, return at line 2434); the same
  absence signal should seed `first_run_help_pending = true`.
- On first launch, after the workspace renders, present the First Steps tab
  once, docked per 3.3. Do not auto-reopen it on later launches once dismissed.
- **Dismissal persistence:** add a field such as `help_first_steps_dismissed` to
  `WorkbenchPreferences` (`ui.hpp:625-644`), serialized in
  `serialize_workbench_preferences()` (`ui.cpp:1499-1521`) and parsed with the
  same fail-closed validation as the rest of the file
  (`parse_workbench_preferences()`, `ui.cpp:1395-1496`;
  `WorkbenchPreferences::validate()`, `ui.cpp:1601`). Bounds: the file already
  caps at `kMaxPreferenceBytes = 16 KiB` (`ui.cpp:50`); one boolean line fits.
  Unknown/corrupt content falls back to defaults with a recorded UI problem,
  matching the existing corrupted-prefs path in `UiController::load_preferences()`
  (`ui.cpp:12211-12260`).
- **Never in project truth:** the flag must not be written to any `.carto` file
  (schema is governed by `Project::kSchemaVersion`, `project.hpp:326`, and
  project serialize at `libs/project/src/project.cpp:5506`), and must not be
  encoded in `workspace-docking.ini` (loaded/saved at `main.cpp:2451-2497` /
  2499+). Those are document and dock-layout state respectively; help dismissal
  is neither. `workbench.prefs` is the established home for exactly this class
  of local presentation preference (see the header comment at `ui.hpp:522-524`).

### 3.5 Panel-level `?` contextual links (must-have for alpha)

Small `?` affordances on key panels (Scene, Inspector, Import/Export
menu area — "Outliner" is not a panel name in this shell) that open the help
tab pre-scrolled to the topic matching that panel
(e.g. Inspector `?` → `help.inspector` topic; Scene `?` → First Steps). These are
in-app navigation actions, not hyperlinks: they jump within the offline help
content. They must follow the same nonmodal rules as 3.3.

### 3.6 Reset behavior (must-have, test-enforced)

- **View > Reset workspace layout** (`apps/desktop/main.cpp:6381-6387`, which
  calls `reset_workbench_layout()` at 2627-2632 and `dock_workspace.request_reset()`
  defined in `apps/desktop/carto_studio_dock.hpp:107-112`) resets instruments,
  flows, and dock topology only. It must **not** reset `help_first_steps_dismissed`.
- **Command palette > Reset Workspace** (`workspace.reset` at `ui.cpp:12111`,
  dispatched at `main.cpp:6500-6503` via `ui.reset_preferences()` +
  `reset_workbench()` at 2673-2680) resets `UiPreferences` (workspace prefs) and
  workbench state. It must also **not** clear the help dismissal: help state lives
  in workbench prefs, and `reset_workbench()` deliberately preserves
  `flow_use_counts`/`last_flow_index` (lines 2674-2679) — the help flag should be
  preserved by the same pattern.
- Rationale: a user who dismissed help and later reset their layout would be
  re-nagged on every launch if dismissal were tied to layout reset.

### 3.7 Offline packaging (must-have for alpha)

- `CMakeLists.txt:280` already installs `docs/` to `${CMAKE_INSTALL_DOCDIR}` —
  `docs/first-use/` exists in this package, so it ships automatically. Keep it
  that way.
- **There is currently no install rule for `examples/`.** Add one (a lane outside
  this handoff's edit scope must do it): install `examples/first-block/` —
  including `first-block.carto`, `first-block.obj`, `manifest.json`, `README.md`,
  and `PROVENANCE.md` — into the documentation/example install tree next to the
  installed docs. `generate_example.py` documents provenance and should ship too.
- The First Steps content must reference the example by a **relative, resolved-at-
  runtime path** (e.g. relative to the executable or the install doc dir), never
  an absolute path and never a download URL. Alpha help is fully offline; if a
  file cannot be found, the help tab says so in one line and still renders the
  text steps.
- First Steps may offer an in-app action "Open example project" that resolves the
  installed `first-block.carto` and runs the normal open path — the same path as
  File > Open (`main.cpp:6250`) — rather than shelling out.

---

## 4. Required focused tests

Existing test conventions to follow: each test is a `add_executable` +
`target_link_libraries` + `carto_target(...)` + `add_test(...)` block in
`tests/CMakeLists.txt` (see `cartographer_ui_tests` at lines 922/1094-1102 and
`cartographer_desktop_workbench_input_tests` at lines 1105-1115). Preference
round-trip and corruption tests live in `tests/ui_tests.cpp` (see
`workbench_preferences_round_trip_and_fail_closed()` at line 1523 and its
corrupt/legacy cases through ~line 1676). ImGui frame-injection interaction tests
live in `tests/desktop_workbench_input_tests.cpp` (its `draw_grip_frame` harness
at the top of the file is the model for frame-level assertions).

Required coverage, one focused case each:

1. **First-run default** — no `workbench.prefs` on disk → help tab shown once on
   first frame batch; `help_first_steps_dismissed` serializes as dismissed only
   after the user closes it. UI-layer: extend the `ui_tests.cpp`
   workbench-preferences round trip.
2. **Dismiss / relaunch** — dismiss, save prefs, reload → tab not shown;
   flag round-trips through `serialize_workbench_preferences` /
   `parse_workbench_preferences` unchanged.
3. **Manual reopen** — with dismissal persisted, Help > First Steps / F1 /
   `?` links reopen the tab; Help menu items switch topic on an already-open tab.
4. **Reset preservation** — with dismissal persisted, run both reset paths
   (`reset_workbench_layout` equivalent and `workspace.reset` dispatch) → flag
   survives; layout/instrument state resets normally.
5. **No modal / no focus steal** — while the help tab is open (including
   immediately after first-launch auto-show), a viewport interaction and a text
   field edit still work; assert no popup is open, no modal flag, and keyboard
   capture state is unchanged. Frame-injection test modeled on
   `desktop_workbench_input_tests.cpp`.
6. **Torn-out-window shortcuts** — with focus in a torn-out OS window (simulated
   via the ImGui-input routing path; see the comment at `main.cpp:6133-6134`),
   F1 still opens help. This must be exercised through the ImGui route, not the
   Win32 route.
7. **Corrupted prefs** — hand-corrupt `workbench.prefs` (truncate, bad field,
   help flag malformed) → fail closed to defaults with a recorded UI problem
   (`ui.cpp:12211-12260` path), help returns to first-run-pending state, and the
   app does not crash or write the flag into project/dock files.

A sweep test worth adding but not a blocker: assert the string
`help_first_steps_dismissed` (or its serialization key) never appears in any
`.carto` serialization path and not in `workspace-docking.ini` content.

---

## 5. Candidate acceptance under Sol's serialized lease

This lane cannot build, launch, or interact with the app; per the alpha process,
**all native behavior is code-checked, never candidate-runtime-accepted**, and
builds/interactions run only under Sol's serialized build/interaction lease.
Acceptance sequence for this handoff's wiring:

1. **Code-check phase (any lane):** the seven focused tests above exist, compile
   as part of the normal test targets, and pass under the standard test runner
   (`carto_target`/`add_test` registration in `tests/CMakeLists.txt`).
2. **Serialized build/interaction lease (Sol only):** Sol runs the desktop build
   and performs the scripted interactions — first launch, dismiss, relaunch,
   manual reopen via Help menu and F1, View > Reset workspace layout, command
   palette Reset Workspace, torn-out-window F1, and a corrupted-`workbench.prefs`
   launch. Each interaction is observed and its outcome recorded against the
   behaviors in sections 3-4.
3. **Acceptance gate:** all seven behaviors verified under the lease, plus the
   packaging check (installed tree contains `docs/first-use/` and
   `examples/first-block/`, resolvable relatively, no absolute paths or download
   URLs in shipped help content). Any failure returns the item to code-check; it
   is not runtime-patched.

## 6. Must-have alpha wiring vs. optional polish

**Must-have for alpha**

- Help menu with its four proposed entries (3.1) and F1 (3.2).
- Nonmodal docked First Steps/Help tab with no focus steal (3.3).
- First-launch-only presentation with dismissal persisted in `workbench.prefs`
  only (3.4).
- Panel-level `?` links on the primary panels (3.5).
- Reset paths preserving dismissal (3.6).
- Offline packaging of `docs/first-use/` + `examples/first-block/` (3.7).
- The seven focused tests (section 4) and lease acceptance (section 5).

**Optional polish (post-alpha)**

- Topic search inside the help tab and a "you are here" panel-to-topic highlight.
- Command-palette entries ("help.first-steps", "help.controls", ...) alongside
  `create.box`/`workspace.reset` in `UiController::command_views()`
  (`ui.cpp:12094+`).
- Translated help strings once the UI picks up localization.
- Extending `enum class Panel` so help becomes a toggleable named panel in View
  menu and workspace prefs (deferred: touches workspace truth and
  `sync_application_workspace`, `ui.cpp:12084-12092`).
- "Open example project" one-click action in First Steps (3.7 describes the
  fallback: instructions only).
- Help-version stamp showing which doc revision shipped with the build.

## 7. Related documents

All of the following exist in the baseline worktree at the paths given:

- `docs/first-use/in-app-help.md` — the help copy/structure spec this wiring
  renders: welcome card, quick start, per-surface topics, contextual error
  hints.
- `docs/first-use/controls.md`, `import-export.md`, `troubleshooting.md`,
  `startup-guide.md` — the companion guides that supply the Controls,
  Import/Export, Troubleshooting, and First Steps topic material.
- `examples/first-block/README.md` and `PROVENANCE.md` — example usage and
  generation provenance (CLI 0.1.0, schema 34).
- `docs/format/cartographer-project-v35.md` — current project schema; the v34
  example is readable per the v1–v35 reader range.
- `docs/architecture/ui.md` — UI layer conventions (presentation vs. project
  truth) that the dismissal-storage decision follows.
- `README.md` — user-facing overview; do not duplicate help content there.
