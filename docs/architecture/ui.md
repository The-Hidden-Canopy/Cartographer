# Cartographer UI foundation

`carto_ui` is Cartographer's authority-preserving presentation layer. It is
deliberately independent of Win32, Dear ImGui, Vulkan, Hub services, and any
private repository. The headless controller is the testable part of the UI
contract; the optional native shell is an adapter over that contract.

## Authority boundary

The supported path is:

```text
input / shortcut / panel action
    -> carto_ui::UiController
    -> carto_application::HumanApplicationAccess
    -> carto_application::ApplicationSession::dispatch
    -> editor command / project validation
    -> immutable ApplicationSnapshot
    -> UiSnapshot presentation projection
```

`UiController` owns only presentation state: theme, density, operator mode,
workspace label, visible panel preferences, command-palette state, the bounded
operation rail, and UI-local diagnostics. It does not own a project document,
editable mesh, render resource, or authority to bypass `ApplicationSession`.
Authoring actions remain application actions and failed actions do not enter
the UI operation rail.

The native input seam is typed at the UI boundary. Win32 key messages become
`NativeKeyEvent` values, and primary viewport clicks become
`NativePointerEvent` values with bounded selection modifiers. The desktop
adapter may perform viewport hit testing against a revision-bound compiled
snapshot, but every resulting selection still enters through the existing
`ApplicationAction`/`UiController` path. No platform callback mutates document
or selection state directly.

The operation rail is lineage metadata for the current UI session. It records
successful project-affecting application receipts with before/after revisions;
it is not a second history, audit ledger, or durable project record. Application
diagnostics remain distinct from UI-local preference/interaction diagnostics.

Persistence status is derived from the application snapshot rather than from
the absence of an error. A project without a bound path is `Not saved`, even
when it is clean; a path-bound clean project is `Saved`, and a dirty bound
project is `Modified`. Failed saves never convert an unbound project into the
saved state. Human authoring dispatch uses the application admission boundary.
Native AI intent follows a separate typed proposal API: the planner can
describe a revision-bound preview, and the host can auto-apply only operations
marked safe by the native ontology. Review mode remains available; either path
submits through the same application admission boundary.

## Public primitives currently implemented

Native Inspector position/transform drafts bind project generation/revision and
the displayed owner/vertex. Clean fields refresh; edited fields remain visible
but stale after a source revision changes, until explicit Reload. Source-scoped
widget IDs also separate active text buffers. Shared UI commit helpers recheck
the live session and selection immediately before dispatch, retaining existing
human command/history/journal behavior. See the
[Inspector draft ledger](../verification/frontend-inspector-drafts.md).

The native studio follows the supplied viewport-first design reference: dark
blue-black surfaces, cyan interaction, bronze identity, a left scene hierarchy,
right Inspector, and bottom Assets/History tabs. A single snapshot-driven style
adapter applies all native colors and Compact/Standard/Touch geometry. It does
not overwrite high-contrast preferences with the dark palette. Header/footer
and dock host share fitted density/font-aware bounds; narrow windows retain
workspace access through a selector. Existing dock IDs and user layouts remain
unchanged. Scene and asset filters are presentation projections over real IDs;
they cannot modify project data or invent thumbnails.
Docked instruments use their tabs for movement; the existing explicit Move/Float
path remains in the panel's context menu. A bounded navigation rail keeps
Project/Settings content visible beside it rather than filling the entire page.

The landscape, GIS, weather, and rendering in the supplied mockup remain visual
references, not implemented runtime claims. See the
[native design ledger](../verification/frontend-design.md) for attachment
provenance, tests, and actual native checks.

- centralized dark, light, and high-contrast color tokens;
- Compact, Standard, and Touch density state;
- Person-First and bounded native AI-First presentation modes;
- Model, Sculpt, CAD, Build, Material, Animate, Review, and AI workspace
  labels, with unsupported workspace capabilities left visible as future
  boundaries rather than fabricated functionality;
- Scene, Viewport, Inspector, Operations, Graph, Assets, Timeline, Problems,
  and Console panel vocabulary;
- bounded command-palette state and keyboard routing for save, undo, redo,
  repeat-last, object/vertex/edge/face modes, operator mode, and overlay close;
- native Win32 key routing plus revision-bound primary viewport pointer
  routing for object, vertex, edge, and face selection;
- a required viewport invariant;
- local, bounded, line-oriented workspace preferences with atomic replacement,
  duplicate/unknown-field rejection, and corrupt-file reset to defaults;
- explicit edge-authoring and unsupported-operation surfaces;
- a native Drafting Board that exposes proposal, preview, apply, and discard
  states without presenting a proposal as authoritative geometry.
- revision-bound repeat-last and adjust-last tool actions that rebind saved
  component identities against the current document before re-evaluation and
  restore the prior selection when a stale identity blocks the replay.
- a bounded editor numeric-entry substrate for finite unit-aware absolute and
  relative scalar values, including bounded arithmetic and grouping; it is not
  yet a modal text-input or gizmo service.

The current application model supports object, vertex, edge, and face
selection. Edge selection is identity- and revision-bound, and is available to
the headless controller and source-wired viewport picker without changing
project truth. A single selected edge can be split through the same
revision-guarded project command path as the other authoring tools; the
operation is undoable, reports its factor, and clears the removed edge
selection. A single selected convex face can likewise be inset through the
same command boundary with distance metadata and guarded undo/redo. Concave
inset and broader persistent edge authoring remain deferred. The Modify
instrument and command palette expose vertex/edge/face conversion, linked,
grow/shrink/invert, and edge boundary/loop/ring selection. Shortest-path
selection requires one start vertex and an explicit owner-bound goal. These
shared UI routes validate project generation/revision and dispatch the existing
selection actions without changing the document, journal, or history. Numeric
mode shortcuts retain their existing clear-selection behavior.
An object-bound selected vertex can slide along a deterministic incident support
edge (or an explicitly supplied support edge) with a bounded factor; the
topology remains unchanged and invalid support/factor/geometry inputs fail
closed.
AI-First uses the native `carto_ai` planner for a deliberately narrow first
workflow. It produces typed proposals for selected-face extrusion/inset and
selected-vertex position changes. The proposal is revalidated against the
current revision and selection before preview and again before commit; raw AI
application actions remain rejected. Default auto-approval is limited to the
native preview-safe operation set. No model service or unrestricted direct AI
mutation path exists.

The Modify instrument also exposes a bounded instance batch: 1–4,096 copies
of one visible, unlocked root mesh with finite translation steps. Command
availability and execution share the same source-eligibility predicate. The
batch enters through one existing application command, journal event, and undo
step. UV/Material controls can generate a unique lightmap UV candidate for one
selected mesh through the human application boundary; the separate Agent draft
path remains available. A generated candidate is not bake-readiness evidence.

Assets exposes exact-byte moved-source review for a registered source in a
saved project. Preparation checks the captured project scope and source digest
and stages immutable bytes in the managed blob store. Confirmation revalidates
the managed path and digest before the application publishes a registry change
through its existing command. Restoring bytes to an already registered managed
path returns an application no-change receipt without a document/history/
journal change. Discarded preparations can leave unreferenced immutable blobs.

Native decal placement captures the displayed target mesh, live/Agent scene
source, and project generation/revision. The shared UI admission helper requires
the live scene and exact displayed/source geometry equality before constructing
an action. Preview switches invalidate the hit cache and cancel armed placement;
application admission independently rejects stale generations and nonhuman
sources. Project format and decal persistence are unchanged.

Repeat and adjust-last retain only the last accepted tool identity, complete
arguments, and value-level authoring context in session state; they are not a
second journal and are not serialized into `.carto`. Replaying after undo or a
document replacement rebinds stable object/component IDs to the current mesh.
Missing, stale, or removed identities fail closed without document mutation.

## Native shell integration

The optional `apps/desktop` source now constructs `UiController`, consumes
`UiSnapshot`, maps the centralized tokens into its ImGui style, loads and saves
bounded local preferences under the Windows user profile, and routes toolbar,
menu, shortcut, palette, workspace, and inspector actions through the
controller/application boundary. The native shell now presents a product-shaped
surface around that contract: persistent navigation for Workspace, Project,
Diagnostics, and Settings; explicit project/revision/persistence state; truthful
native-shell and viewport capability cards; and a settings surface for the
bounded UI preferences. The Workspace surface is a drafting workbench hosted
beneath its fixed header. Every workbench pane and instrument is a dockable
ImGui window: panes can be split, tabbed, hidden, resized, moved freely, or
dragged beyond the main window to become native OS windows, then docked back
into the workspace. The docked Viewport window is bound to the persisted
work-surface pane flag: closing its tab — or its torn-out native OS window —
hides it without touching project, selection, cameras, view state, or the dock
tree, and it restores to the same dock node through View > Work surface or the
in-workspace "Viewport hidden / Restore viewport" card. Entering focus mode
(F11) restores a hidden work surface so the workspace stays usable afterwards. The header is outside the dockspace. The pinned Dear ImGui
docking branch supplies Win32 and Vulkan multi-viewport support. Dock layout is
persisted per arrangement in bounded, atomically replaced INI files under
`%LOCALAPPDATA%/Cartographer/`: the legacy `workspace-docking.ini` remains the
Studio arrangement's file so existing users keep their saved layout untouched,
while Scene and Review persist to sibling files beside it. The selected
arrangement marker and the viewport view settings live in a separate bounded
atomic `workspace-viewport.prefs`; validated pane
visibility, geometry, and Ledger collapse preferences remain in
`workbench.prefs`. All of these are local UI state and never enter project
truth. Three named arrangements share one dock root and panel set: Studio keeps
the legacy proportions (Scene 17%, Inspector 22%, bottom 21%), Scene narrows the
side and bottom rails for a substantially larger viewport, and Review folds the
panel columns into one left tab group so the viewport takes the remaining
surface. Switching flushes the outgoing arrangement's layout, loads the target's
saved layout when one exists, and otherwise rebuilds that arrangement's authored
topology; re-selecting the active arrangement is a no-op. Switching never
changes project, scene, selection, or camera state and never clears focus mode.
A rejected (corrupt or unreadable) arrangement layout is never overwritten by
shutdown saves: the shell rebuilds that arrangement's authored defaults and
keeps the rejection guard until the artist explicitly resets the workspace;
switching to a healthy or not-yet-created arrangement clears the stale guard so
that arrangement can persist.
The Measure bay is connected to the public surface-fidelity compiler as a
Surface Fidelity instrument: it compiles an explicit triangle-budget preview
from the selected viewport instance, displays the appearance-conservation
receipt, and projects an admitted or rejected ghost representation over the
work surface. The preview is read-only, carries source revision and project
generation provenance, and is removed from the viewport when selection or
authored state becomes stale. Because this compiler currently runs on the
native UI path, the boundary rejects source or target meshes above the
explicit 4,096-triangle CPU preview budget instead of allowing an unbounded
interaction stall; larger workloads remain an offline/renderer-owned concern.
Workspace availability and default layouts are supplied by the validated
`workspace_registry`; the shell does not maintain a second hardcoded list.
Available Model and AI workspaces apply their registry defaults through the
validated preference boundary. Future workspaces retain explicit reasons and
fail closed when selected rather than silently presenting an incomplete mode.
Ledger receipts are selectable and expose the retained operation id, action,
revision interval, and document-change flag. Parameter reopening is explicitly
disabled until the application receipt contract carries an editable parameter
payload; the shell does not infer or fabricate one.
The viewport itself binds named views and display modes as presentation-only
controls. The 3D Perspective view renders through the existing bounded
`EditorCamera`; Top (Plan X/Z) renders through the existing
`WorkSurfaceCamera`; Front (X/Y) and Right (Z/Y) render through the same
`EditorCamera` with bounded orthographic orientation presets, so no separate
renderer mode exists. Each view retains its own camera pose: switching views
captures the outgoing pose and restores the incoming one, and the current
view's pose persists across restarts through the bounded view-settings codec in
`workspace-viewport.prefs`. Scene, selection, materials, and lighting are never
reset by a view change. The fixed orthographic presets cannot be rotated: the
Orbit control is disabled in Front and Right with an explicit tooltip, and the
pointer path refuses orbit input for fixed views, so a bounded 2D projection is
never silently changed. Shaded draws the scene with its existing material
fills; Structure drops fills to show real mesh topology edges; Depth shades
fills by normalized view depth of the displayed geometry. Structure and Depth
are CPU painter re-projections and are labeled as such — the studio performs no
GPU depth or texture readback, and the capability line under the display
controls states this explicitly. Full color and a Rec. 709 monochrome transform
are the two color modes; selection and diagnostic accents stay semantic. The
GPU scene preview binds authored or perspective viewport cameras only, so the
display controls carry an explicit note that orthographic named views render on
the CPU painter.
Unavailable capabilities remain explicit placeholder instruments. Preference
read/write failures are surfaced rather than silently discarded. The shell
remains behind the existing Vulkan SDK and pinned Dear ImGui prerequisites. A
local Debug build and responsive native-window launch have been exercised
against those prerequisites; native message routing is source- and contract-
verified. Docking and viewport-tear-out are integrated against the pinned
docking branch. Populated native view switching, detach/dock, resize, focus,
and dirty-save/reopen are recorded in the
[viewport workspace ledger](../verification/viewport-workspace/EVIDENCE_LEDGER.md).
Secondary-monitor recovery, arbitrary DPI combinations, authored-camera GPU
preview, and broad GPU compatibility remain separate acceptance gates.

## Evidence

`tests/ui_tests.cpp` covers preference invariants, required viewport behavior,
authoritative action routing, operation lineage, persistence-state truth,
AI-proposal admission failure, native proposal/preview/apply lineage,
edge-selection non-mutation, unsupported-operation disclosure, bounded command-palette input,
native key and pointer modifier contracts,
workspace registry identity, default-layout application, and unavailable-workspace rejection,
preference persistence, corrupt-preference reset, validated workbench geometry
round-trips, atomic workbench writes, and failed action/shortcut diagnostics.
It also covers explicit surface selection, closure preview admission, compiled
preview identity, no-authoring-mutation behavior, and stale-preview
invalidation.
These are model and contract tests, not pixel snapshots. Native visual
acceptance remains a separate gate.

`tests/frontend_selection_wiring_tests.cpp` and
`tests/frontend_asset_wiring_tests.cpp` exercise the public UI entry points,
including stale scope, wrong selection, unsupported sources, batch bounds,
exact-byte relinking, command/journal behavior, undo/redo, and save/reopen.
The decal tests also cover same-revision project replacement and rejection of
synthetic altered display geometry; public SculptStack authoring does not
evaluate the private sculpt runtime. See the
[wiring repair ledger](../verification/frontend-backend-wiring.md) for executed
checks and platform-specific gaps.

`tests/desktop_workbench_input_tests.cpp` covers the named dock arrangements
(viewport centrality and share, focus-mode interaction, per-arrangement layout
paths, re-selection no-op, and restart restore of a customized arrangement),
and `tests/desktop_viewport_view_tests.cpp` covers the named-view axes,
orthographic presets, per-view pose capture/restore, monochrome luma mapping,
depth normalization, structure-edge collection, capability labels, and the
bounded view-settings codec. These are model and contract tests; the
[viewport workspace ledger](../verification/viewport-workspace/EVIDENCE_LEDGER.md)
separately records visible-window populated-scene, view-switching,
detach/dock, resize, focus, per-arrangement restoration, and dirty-save/reopen
acceptance. Multi-monitor recovery, arbitrary DPI combinations, and
authored-camera GPU preview remain separate gates.

## Deliberate non-claims

The Model workspace routes a bounded human viewport stroke on one selected
face through the persistent knife-cut command. The face selection and project
revision stay bound during the drag; both endpoints snap to distinct existing
face vertices. Native pointer/click-through acceptance is unverified, and
cross-face or edge-interior cuts remain unsupported.

This layer does not implement the complete UI specification. The desktop
Transform instrument exposes a bounded modal for world-X/Y/Z position entry;
its preview is read-only, revision-bound, and confirmation uses the existing
project transform command. Native text-entry and click-through acceptance are
not yet recorded. Concave inset, render projection/ray generation, visual
target markers beyond the revision-bound fidelity overlay, broad surface/
normal snapping, broader persistent edge authoring/traversal, general-purpose
3D manipulators, numeric rotation/scale, full direct-modeling interaction,
CAD/BUILD/material/animation subsystems, and model-backed AI planning remain
roadmap work. Docking and native viewport tear-out are implemented in the
shell; multi-monitor recovery, arbitrary DPI combinations, and broad GPU
acceptance remain pending.
