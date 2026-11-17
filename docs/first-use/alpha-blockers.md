# Alpha first-use blockers

> Historical handoff from 2026-10-28, recovered during worktree consolidation. Workstation paths use placeholders. UI labels, source line references and unimplemented proposals describe that dated candidate. They are not verification of the current application. See [the current texturing verification](../verification/texturing-workflow.md) and [integration ledger](../verification/worktree-integration-20261116.md).


Prioritized list of what must be resolved before the alpha first-use package is
honest to ship. Every code-derived claim below is traced to source at commit
`fae4f7d95259ba38e79edc94ae12275df4161cd4` ("Import static GLTF scenes through
governed product-bound history"). The only runtime observations so far are the
headless CLI 0.1.0 runs recorded in `evidence.md`, which used a pre-existing
released binary — not a build of this revision — and are not candidate
acceptance of anything.

**Label key.** **code-derived** — visible in source at the pinned revision, not
yet exercised on a candidate build; **unverified acceptance** — cannot be
checked until the candidate build is actually launched; **declared
limitation** — a documented scope limit (section B), not a defect. Nothing in
this document is a candidate runtime result.

Companion documents: `startup-guide.md` (first-run walkthrough) and
`evidence.md` (evidence checklist, including the CLI 0.1.0 observations and
their limits). The example project lives in `examples/first-block/`.

---

## A. Release-blocking wiring and workflow defects

These are wiring gaps: the building blocks exist but are not connected, or the
default path strands a first-time user. Items 1–6 are code-derived; item 7 is
the unverified-acceptance gate that closes them all out.

### 1. No Help or onboarding exists, and dismissed help would be unreachable

- **User impact.** A first-time user has no in-app explanation of Focus flow,
  the command palette, material regions, save discipline, or recovery. On day
  one they must already know Cartographer's conventions or they stop at the
  first blank viewport. Once any future onboarding dialog is dismissed there is
  no Help surface to reopen it — because nothing exists today, there is
  nothing to reopen.
- **Evidence (code-derived).** No Help menu, onboarding panel, or help window
  is registered anywhere in the desktop shell: a case-insensitive search of
  `apps/desktop/main.cpp` and `apps/desktop/carto_studio_dock.hpp` for
  help/onboarding entry points returns no matches. The only discovery
  affordance is the command palette (`apps/desktop/main.cpp:5340`, "Command
  palette  Ctrl+K"); exiting the default Focus flow is keyboard- or
  menu-only (F11 / Ctrl+Shift+V or Escape at `main.cpp:6126`–`6132`, and
  View > "Viewport focus / restore" at `main.cpp:6377`–`6379`) — none of
  which teaches first-use concepts.
- **Responsible surface.** UI shell (`apps/desktop/`) plus whatever help
  content ships with the package (docs or in-app). The startup guide in
  `docs/first-use/` is the content source of truth for the alpha.
- **Required resolution.** Ship a first-run help/onboarding surface: at minimum
  a dismissible first-run panel that names Focus flow, Ctrl+K, and material
  regions, plus a persistent way to reopen it after dismissal (Help menu or
  "Show first-run tips again"). Offline-packaged copy is covered by item 6.
- **Acceptance criterion.** On a clean machine with no prior state, the alpha
  build shows first-use guidance on launch; after dismissing it, the user can
  reopen the same content from a menu; with no network access the content
  renders.

### 2. First-run Focus flow hides the creation instruments

- **User impact.** The default first-run workbench is Focus mode, which hides
  the mode bar and footer (`carto_studio_dock.hpp:234`–`235`) and removes
  dock-space visibility (`carto_studio_dock.hpp:258`–`261`). A new user's
  obvious question — "how do I make a box?" — has no visible answer: no
  dedicated creation instrument is rendered in the default Focus layout.
  Creation is reachable without leaving Focus via the command palette
  (Ctrl+K > Create Box, `main.cpp:5340`), but that path is undiscoverable —
  nothing on screen names it — so the intended first-run creation flow is an
  invisible feature.
- **Evidence (code-derived).** The shell's default workbench flow is Focus
  (`WorkbenchFlow workbench_flow = WorkbenchFlow::focus;`,
  `apps/desktop/main.cpp:1802`). `focus_mode_` defaults to `false` at
  construction (`carto_studio_dock.hpp:111`, member default at `:299`), and the
  shell turns Focus on by default rather than the dock default doing it. In
  Focus mode `reserved` and `footer` heights become 0 and dock visibility is
  gated (`carto_studio_dock.hpp:234`–`235`, `258`–`261`). No dedicated
  creation control is rendered in the Focus viewport; the command palette
  (`main.cpp:5340`) is the only creation entry point reachable without exiting
  Focus (Ctrl+K > Create Box works in Focus), and the Focus exit paths
  (`main.cpp:6126`–`6132`, `6377`–`6379`) only unhide instruments — they do not
  put a labeled create-box control on screen.
- **Responsible surface.** First-run workbench flow in `apps/desktop/main.cpp`
  (default at `main.cpp:1802`; flow table at `main.cpp:1111`) and the dock in
  `apps/desktop/carto_studio_dock.hpp`.
- **Required resolution.** Either default new users to a flow where creation is
  visible, or make Focus mode's exit/create affordance self-describing (a
  labeled control, not a shortcut only). First-run onboarding (item 1) can
  carry part of this, but the instrument must still be reachable without
  memorizing a shortcut.
- **Acceptance criterion.** A first-time user, with no outside documentation,
  can create a box within the first two minutes of launch.

### 3. Newly created primitives have no material region to assign to

- **User impact.** The natural first session is "create a box, give it a
  material." `Create Box` produces a mesh with no material and no face
  material-region layer, so the Material instrument has nothing to assign
  against. The user's first material assignment — the step the first-block
  example is built around — cannot be performed on the content they just made;
  it only works on geometry that declares regions (the example's OBJ carries
  `CARTOGRAPHER_FACE_MATERIAL` region 1 on every one of its 6 faces). New
  users will conclude the Material instrument is broken.
- **Evidence (code-derived).** `ApplicationSession::create_box`
  (`libs/application/src/application.cpp:5069`–`5080`) calls
  `geometry::make_box` and wraps it in `CreateMeshObjectCommand` — no
  material-region seeding anywhere in the path; `create_plane`
  (`application.cpp:5082`–`5095`) is identical. Empty projects likewise start
  with no materials and no face material-region layer.
- **Responsible surface.** `libs/application` (creation commands), possibly
  `libs/editor` (`CreateMeshObjectCommand`), with UI copy in
  `apps/desktop/main.cpp` Material instrument.
- **Required resolution.** Have creation commands seed a default face
  material-region layer (e.g. region 1 on all faces of the new mesh), or
  surface an explicit, guided "add material region" step in the Material
  instrument when none exists. Document whichever behavior ships in the
  startup guide.
- **Acceptance criterion.** A new user can create a box and assign a new
  material to it in one continuous flow with no imported asset, and the
  exported/rendered result shows the material.

### 4. No save-and-close choice; closing a dirty project forces a decision the
   user may not understand

- **User impact.** Native export requires a saved, unmodified `.carto` — the
  shell blocks export of a dirty project ("Save the .carto project before
  exporting a glTF asset.", `main.cpp:4771`; the mesh variant at
  `main.cpp:4952`) — so saving is on the critical path of the first session.
  With no autosave and no previous-version backup, an accidental close of a
  dirty project either discards work or forces a save the user did not plan. A
  first-time user does not yet know the save/export coupling and can lose their
  only session's work in one click.
- **Evidence (code-derived).** There is no autosave: no autosave/auto-save/backup
  path exists in `libs/application`, `libs/ui`, or `apps/desktop`. Dirty
  project close/open paths gate on `snapshot.dirty` and route through
  `discard_dirty = true` actions (`apps/desktop/main.cpp:5437`–`5447`,
  `6245`–`6254`) with a two-way "Discard Unsaved Changes" prompt
  (`main.cpp:5120`–`5126`: "This project has unsaved changes." /
  "Discard them and continue?" / "Discard" — no Save branch). Window close
  (`WM_CLOSE`, `main.cpp:20775`–`20776`) lands in the same two-way prompt via
  `process_close_request` (`main.cpp:5103`–`5112`). Startup recovery itself
  refuses to discard unsaved changes (`libs/application/src/application.cpp:
  4038`–`4042`) and the session requires explicit discard confirmation
  (`application.cpp:4192`–`4198`). No "Save and close" /
  "Close without saving" three-way choice is offered at the shell level.
- **Responsible surface.** `apps/desktop/main.cpp` close/new/open flow;
  recovery policy in `libs/application/src/application.cpp`.
- **Required resolution.** Offer an explicit three-way choice on closing a dirty
  project: Save and close, Close without saving, Cancel. Keep the discard
  confirmation for startup recovery. State the no-autosave, no-backup policy
  plainly in the startup guide so the choice is informed.
- **Acceptance criterion.** Closing a dirty project presents Save and close /
  Close without saving / Cancel; each branch does exactly what its label says,
  verified on the candidate build.

### 5. Recovery expectations are not communicated

- **User impact.** There is no autosave and no previous-version backup file.
  The only safety net is an explicit **File > Recover latest journal snapshot**
  (`apps/desktop/main.cpp:6270`). If a first-time user does not know this exists
  — and nothing today tells them — a crash means they believe their work is
  gone even when a journal snapshot is available. The recovery action also
  discards unsaved changes by design (`action.discard_dirty = true`,
  `main.cpp:5083`–`5084`), which must be explained before use, not discovered
  after.
- **Evidence (code-derived).** Recovery inspection states in
  `libs/application` with UI dialog at `apps/desktop/main.cpp:5075`–`5078`
  ("No pending journal snapshot is available for this project."). The action is
  only reachable through the File menu; nothing in first-run flow mentions it.
- **Responsible surface.** Recovery UX in `apps/desktop/main.cpp`; journal
  snapshot mechanics in `libs/journal` / `libs/application`; documentation in
  `docs/first-use/`.
- **Required resolution.** Tell the user in onboarding and the startup guide:
  what the journal snapshot is, that recovery is manual (File > Recover latest
  journal snapshot), that it discards unsaved changes, and that no autosave or
  backup exists. When no snapshot exists, the existing "No pending journal
  snapshot" dialog should also say what that means for lost work.
- **Acceptance criterion.** A first-time user can state, from in-app or
  packaged help alone, how to recover work after a crash and what recovery
  will not preserve.

### 6. No install or packaging rule ships the example; alpha package assembly is
   undefined

- **User impact.** The startup guide and help content point first-time users at
  `examples/first-block/`. If the alpha ships without that example (and without
  the docs) installed at known locations, a new user following the guide hits a
  dead end — and any future in-app help has nothing to resolve at runtime.
  Help that silently assumes paths only present in a source checkout is not
  offline help.
- **Evidence (code-derived).** `CMakeLists.txt:280` has
  `install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DOCDIR})` — a
  pre-existing rule at this baseline — so `docs/first-use/` ships once that
  install step runs. But no install rule anywhere references `examples/`; the
  five example files plus `generate_example.py` have no packaging path. There
  is also no CPack configuration or any other packaging definition, so how the
  alpha first-use package is assembled and distributed is itself undefined.
  Nothing in the app references the docs or the example by a runtime-resolvable
  relative path (there is no help surface at all yet — item 1).
- **Responsible surface.** Build/packaging (`CMakeLists.txt`, new packaging
  definition); the help surface in `apps/desktop/` once it exists (item 1) for
  runtime path resolution.
- **Required resolution.** Add an install rule that ships `examples/first-block/`
  (`first-block.carto`, `first-block.obj`, `manifest.json`, `README.md`,
  `PROVENANCE.md`, and `generate_example.py`) into the install tree next to the
  installed docs, and define the alpha packaging step that produces the
  shippable package. All help and guide references to docs and the example must
  use relative paths resolved at runtime — never absolute paths, never download
  URLs; the package must work with no network access.
- **Acceptance criterion.** Running the documented packaging/install step for
  the candidate produces an installed tree that contains the complete
  `docs/first-use/` and `examples/first-block/`; help content resolves the
  example by a relative runtime path; and all of it is usable offline.

### 7. No end-to-end candidate acceptance has been observed under Sol's lease

- **User impact.** Native behavior is code-derived or observed through a
  pre-existing CLI binary, not watched on a candidate build. Alpha ships a
  promise about a runtime nobody has seen a first-time user (or scripted
  stand-in) complete. Without one observed pass of the documented first-use
  route — import the First Block example, select and frame it, perform a move
  edit, create and assign a material, save, close, cold-reopen, export, plus
  an explicit journal-snapshot recovery check — the package cannot claim the
  route works.
- **Evidence (unverified acceptance).** All UI behavior above is code-derived.
  The headless re-validation of the example asset (`evidence.md` O1–O5) was
  observed only with the pre-existing CLI 0.1.0 binary, not a build of this
  revision, and is not candidate GUI acceptance. Open questions only runtime
  answers: the schema-34 example opening and re-saving under the schema-35
  candidate (reader accepts v1–v35: `kSchemaVersion = 35` at
  `libs/project/include/carto/project/project.hpp:326`,
  `kMinimumReadableSchemaVersion = 1` at `:327`; example header
  `CARTOGRAPHER_PROJECT 34`), Focus flow behavior in item 2, the save/export
  dialog text at `apps/desktop/main.cpp:4771` and `4952`, and recovery dialog
  behavior.
- **Responsible surface.** Release/verification lane against the Vulkan
  workbench candidate (`apps/desktop`; the D3D12 shell is out of scope here),
  using Sol's serialized build/interaction lease. All native behavior is
  code-checked, not candidate runtime-accepted, until this item closes.
- **Required resolution.** Run the full first-use route on the candidate build
  under Sol's lease — import First Block, select/frame, move edit, create and
  assign a material, save, close, cold reopen, export, plus an explicit
  journal-snapshot recovery check — and record observed results in
  `docs/first-use/evidence.md`. Any step that fails converts to a code fix,
  not a documentation workaround.
- **Acceptance criterion.** Every checklist line in the release checklist below
  is marked observed-pass or observed-fail with a run id, not merely
  code-reviewed.

---

## B. Documented limitations that need not block release

These are **declared limitations** — declared scope limits, not defects —
provided they stay declared in the startup guide and user-facing copy. If any
is discovered to be undeclared or misleading in-product, it promotes to
section A.

| Limitation | Why it need not block | Must be declared where |
| --- | --- | --- |
| The Vulkan workbench's only viewport is a 2D orthographic X/Z plan view; this shell has no 3D orbit route | Every first-use step (pan/zoom, create, select, assign material, save, export) is carried by the 2D plan surface, so no authoring claim in the first-use route depends on a 3D viewport | Startup guide, viewport label |
| Geometry-only OBJ export | Export of the example block (vertices, faces, region comments) round-trips — observed with the pre-existing CLI 0.1.0 (`evidence.md` O3–O4), not the candidate; materials/textures are out of OBJ's scope here | Export dialog or guide |
| Example project is schema 34 while current source writes schema 35 | The reader accepts v1–v35 (`project.hpp:326`–`327`, code-derived); saving upgrades the file, which the example README already documents | Example README (done), startup guide |

---

## C. Release checklist — do not check until observed

Each line requires an observed pass on the candidate build under Sol's lease.
"Observed" means a run was executed and its result recorded with a run id;
code review alone does not count, and no line is marked passed today.

- [ ] Launch with no prior state: first-run guidance appears and is re-openable
      after dismissal (item 1).
- [ ] From a blank viewport, a new user path to Create Box is visible without
      outside documentation; Ctrl+K > Create Box also works (item 2).
- [ ] Create Box produces an object a material can be assigned to in one
      continuous flow (item 3).
- [ ] Closing a dirty project offers Save and close / Close without saving /
      Cancel and each branch behaves as labeled (item 4).
- [ ] File > Recover latest journal snapshot recovers after a simulated crash
      and states what it discards; with no snapshot, the dialog says work is
      unrecoverable (item 5).
- [ ] The documented packaging/install step produces an installed tree with
      complete `docs/first-use/` and `examples/first-block/`, resolvable by
      relative runtime paths, fully usable offline (item 6).
- [ ] `examples/first-block/first-block.carto` (schema 34) opens in the
      schema-35 candidate, accepts a material on region 1, and re-saves.
- [ ] Native export is accepted only from a saved, unmodified `.carto`, and
      the blocking dialog text matches the documented rule.
- [ ] The full documented first-use route completes end-to-end on the
      candidate build under Sol's lease, with results recorded in
      `docs/first-use/evidence.md` (item 7).

---

*Scope note: this document covers the Vulkan workbench (`apps/desktop`) only.
The D3D12 shell is a separate surface and is not evaluated here.*
