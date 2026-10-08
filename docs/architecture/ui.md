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
The public controller exposes no private-service proposal or mutation API.
AI-First remains a truthful unavailable-state layout, and the Drafting Board is
local notes only unless an out-of-tree private build supplies its own adapter.

## Public primitives currently implemented

- centralized dark, light, and high-contrast color tokens;
- Compact, Standard, and Touch density state;
- Person-First and AI-First presentation modes, with private service
  availability reported explicitly;
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
- a native Drafting Board that retains local, non-authoritative notes without
  sending them or exposing a public service binding;
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
inset, broader persistent edge authoring, and traversal tools remain deferred.
An object-bound selected vertex can slide along a deterministic incident support
edge (or an explicitly supplied support edge) with a bounded factor; the
topology remains unchanged and invalid support/factor/geometry inputs fail
closed.
AI-First does not fabricate service availability. The public build displays the
private-service boundary and contains no planner, provider client, operation
catalog, or service-to-project mutation path.

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
Workspace availability and default layouts are supplied by the validated
`workspace_registry`; the shell does not maintain a second hardcoded list.
Available Model and AI workspaces apply their registry defaults through the
validated preference boundary. Future workspaces retain explicit reasons and
fail closed when selected rather than silently presenting an incomplete mode.
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
workspace registry identity, default-layout application, and unavailable-workspace rejection,
preference persistence, corrupt-preference reset, validated workbench geometry
round-trips, atomic workbench writes, and failed action/shortcut diagnostics.
These are model and contract tests, not pixel snapshots. Native visual
acceptance remains a separate gate.

## Deliberate non-claims

This layer does not implement the complete UI specification. Concave inset,
visual target markers, viewport projection, broad surface/normal snapping, broader persistent edge authoring/traversal, gizmos, numeric
transform editing, full modal numeric confirmation/axis locks, full direct-modeling interaction, CAD/BUILD/material/
animation subsystems, model-backed AI planning, full docking/tab stacks,
OS-level tear-out, native visual/click-through/DPI acceptance, and broad GPU compatibility
remain roadmap work.
