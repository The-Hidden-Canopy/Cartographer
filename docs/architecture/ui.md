# Cartographer UI foundation

`carto_ui` is the authority-preserving presentation layer for the VANTA-family
shell described by the UI/UX specification. It is deliberately independent of
Win32, Dear ImGui, Vulkan, and any private repository. The headless controller
is the testable part of the UI contract; the optional native shell is an
adapter over that contract.

## Authority boundary

The supported path is:

```text
input / shortcut / panel action
    -> carto_ui::UiController
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

The operation rail is lineage metadata for the current UI session. It records
successful project-affecting application receipts with before/after revisions;
it is not a second history, audit ledger, or durable project record. Application
diagnostics remain distinct from UI-local preference/interaction diagnostics.

## VANTA-family primitives currently implemented

- centralized dark, light, and high-contrast color tokens;
- Compact, Standard, and Touch density state;
- Person-First and explicitly unavailable AI-First presentation modes;
- Model, Sculpt, CAD, Build, Material, Animate, Review, and AI workspace
  labels, with unsupported workspace capabilities left visible as future
  boundaries rather than fabricated functionality;
- Scene, Viewport, Inspector, Operations, Graph, Assets, Timeline, Problems,
  and Console panel vocabulary;
- bounded command-palette state and keyboard routing for save, undo, redo,
  object/vertex/edge/face modes, operator mode, and overlay close;
- a required viewport invariant;
- local, bounded, line-oriented workspace preferences with atomic replacement,
  duplicate/unknown-field rejection, and corrupt-file reset to defaults;
- explicit edge-authoring and "AI tools unavailable" surfaces.

The current application model supports object, vertex, edge, and face
selection. Edge selection is identity- and revision-bound, and is available to
the headless controller and source-wired viewport picker without changing
project truth. Persistent edge authoring, traversal tools, and edge-edit
transactions remain deferred. AI-First is a presentation shell only: there is
no planner, proposal transaction adapter, model service, or direct AI mutation
path in this repository.

## Native shell integration

The optional `apps/desktop` source now constructs `UiController`, consumes
`UiSnapshot`, maps the centralized tokens into its ImGui style, loads and saves
bounded local preferences under the Windows user profile, and routes toolbar,
menu, shortcut, palette, workspace, and inspector actions through the
controller/application boundary. Preference read/write failures are surfaced
rather than silently discarded. The source remains behind the existing Vulkan
SDK and pinned Dear ImGui prerequisites. This checkout does not claim a native
build, launch, GPU execution, input, DPI, resize, or desktop workflow result
until those prerequisites are present and exercised.

## Evidence

`tests/ui_tests.cpp` covers preference invariants, required viewport behavior,
authoritative action routing, operation lineage, edge-selection non-mutation,
AI-unavailable disclosure, bounded command-palette input,
preference persistence, corrupt-preference reset, and failed action/shortcut
diagnostics. These are model and contract tests, not pixel snapshots. Native
visual acceptance remains a separate gate.

## Deliberate non-claims

This layer does not implement the complete UI specification. Persistent edge
authoring, gizmos, snapping, numeric transform editing, full direct-modeling
interaction, CAD/BUILD/material/animation subsystems, AI planning/proposals,
workspace docking, and native Vulkan/ImGui acceptance remain roadmap work.
