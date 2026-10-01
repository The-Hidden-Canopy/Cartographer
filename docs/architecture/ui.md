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

- centralized dark, light, and high-contrast color tokens;
- Compact, Standard, and Touch density state;
- Person-First and bounded native AI-First presentation modes;
- Model, Sculpt, CAD, Build, Material, Animate, Review, and AI workspace
  labels, with unsupported workspace capabilities left visible as future
  boundaries rather than fabricated functionality;
- Scene, Viewport, Inspector, Operations, Graph, Assets, Timeline, Problems,
  and Console panel vocabulary;
- bounded command-palette state and keyboard routing for save, undo, redo,
  object/vertex/edge/face modes, operator mode, and overlay close;
- native Win32 key routing plus revision-bound primary viewport pointer
  routing for object, vertex, edge, and face selection;
- a required viewport invariant;
- local, bounded, line-oriented workspace preferences with atomic replacement,
  duplicate/unknown-field rejection, and corrupt-file reset to defaults;
- explicit edge-authoring and unsupported-operation surfaces;
- a native Drafting Board that exposes proposal, preview, apply, and discard
  states without presenting a proposal as authoritative geometry.

The current application model supports object, vertex, edge, and face
selection. Edge selection is identity- and revision-bound, and is available to
the headless controller and source-wired viewport picker without changing
project truth. A single selected edge can be split through the same
revision-guarded project command path as the other authoring tools; the
operation is undoable, reports its factor, and clears the removed edge
selection. A single selected convex face can likewise be inset through the
same command boundary with distance metadata and guarded undo/redo. Concave
inset, broader persistent edge authoring, and traversal tools remain deferred.
AI-First uses the native `carto_ai` planner for a deliberately narrow first
workflow. It produces typed proposals for selected-face extrusion/inset and
selected-vertex position changes. The proposal is revalidated against the
current revision and selection before preview and again before commit; raw AI
application actions remain rejected. Default auto-approval is limited to the
native preview-safe operation set. No model service or unrestricted direct AI
mutation path exists.

## Native shell integration

The optional `apps/desktop` source now constructs `UiController`, consumes
`UiSnapshot`, maps the centralized tokens into its ImGui style, loads and saves
bounded local preferences under the Windows user profile, and routes toolbar,
menu, shortcut, palette, workspace, and inspector actions through the
controller/application boundary. The native shell now presents a product-shaped
surface around that contract: persistent navigation for Workspace, Project,
Diagnostics, and Settings; explicit project/revision/persistence state; truthful
native-shell and viewport capability cards; and a settings surface for the
bounded UI preferences. The Workspace surface is a drafting workbench: the
viewport is the fixed work surface, the left and right edges are Tool Rack and
Instrument Bay handles, authoring controls open as movable/resizable instrument
windows with visible grip spines, and the bottom surface is an Operation
Ledger. Instruments may be opened, closed, repositioned, resized, and seated
near the racks; nearby instrument edges magnetically settle into aligned
adjacent positions, and dropping one onto a rack returns it to storage.
Instrument visibility, geometry, and Ledger collapse state are validated and
atomically persisted as local `workbench.prefs` state, separate from project
truth.
Ledger receipts are selectable and expose the retained operation id, action,
revision interval, and document-change flag. Parameter reopening is explicitly
disabled until the application receipt contract carries an editable parameter
payload; the shell does not infer or fabricate one.
Unavailable capabilities remain explicit placeholder instruments. Preference
read/write failures are surfaced rather than silently discarded. The shell
remains behind the existing Vulkan SDK and pinned Dear ImGui prerequisites. A
local Debug build and responsive native-window launch have been exercised
against those prerequisites; native message routing is source- and contract-
verified, while click-through, pixel, DPI, resize,
OS-level tear-out, and broad GPU compatibility remain separate acceptance
gates.

## Evidence

`tests/ui_tests.cpp` covers preference invariants, required viewport behavior,
authoritative action routing, operation lineage, persistence-state truth,
AI-proposal admission failure, native proposal/preview/apply lineage,
edge-selection non-mutation, unsupported-operation disclosure, bounded command-palette input,
native key and pointer modifier contracts,
preference persistence, corrupt-preference reset, validated workbench geometry
round-trips, atomic workbench writes, and failed action/shortcut diagnostics.
These are model and contract tests, not pixel snapshots. Native visual
acceptance remains a separate gate.

## Deliberate non-claims

This layer does not implement the complete UI specification. Concave inset,
broader persistent edge authoring/traversal, gizmos, snapping, numeric
transform editing, full direct-modeling interaction, CAD/BUILD/material/
animation subsystems, model-backed AI planning, full docking/tab stacks,
OS-level tear-out, native visual/click-through/DPI acceptance, and broad GPU compatibility
remain roadmap work.
