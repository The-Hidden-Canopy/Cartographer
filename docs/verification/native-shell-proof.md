# Native shell proof record

This record separates native shell evidence from the headless contract and
from claims about broad GPU or desktop compatibility.

## Scope

- Product surface: `apps/desktop/cartographer_desktop.exe`
- Shell boundary: `carto_ui::UiController` and immutable `UiSnapshot`
- Authoring boundary: `carto_application::ApplicationSession`
- Renderer boundary: optional Vulkan surface used by the native window

## Evidence exercised locally

- The native target configured with an installed Vulkan SDK, pinned Dear ImGui
  source, `CARTO_ENABLE_VULKAN=ON`, and `CARTO_BUILD_DESKTOP=ON`.
- `cmake --build build/native --parallel 4` completed successfully after the
  product-shell change.
- `ctest --test-dir build/native --output-on-failure` passed all 8 registered
  test targets.
- The rebuilt executable launched from `build/native/apps/desktop` and exposed
  a responsive window titled `Cartographer` after startup.
- The shell presents Workspace, Project, Diagnostics, and Settings surfaces;
  domain actions continue to dispatch through `UiController` rather than
  mutating application state from a panel.
- The Workspace surface presents a fixed work surface, left Tool Rack, right
  Instrument Bay, movable/resizable instrument windows with grip spines, and a
  revision-bound Operation Ledger.
- Instrument release behavior includes bounded magnetic seating against nearby
  instrument edges and rack-overlap storage; it does not claim full docking or
  tab-stack behavior.
- The Ledger timeline is selectable and displays the selected receipt's action,
  revision interval, and document-change flag. Reopening parameters is visibly
  disabled because the current receipt contract does not retain parameter
  payloads.
- Workbench visibility, instrument geometry, rack storage, Ledger collapse
  state, bounded parsing, and atomic local persistence are covered by the UI
  contract tests. The native shell loads those preferences at startup and
  publishes them on clean shutdown.

## Not established by this record

- pixel-perfect visual acceptance or screenshot comparison;
- full native click-through coverage, because the current computer-use bridge
  did not expose the native window for interaction;
- OS-level tear-out and full docking/tab-stack behavior;
- DPI, resize, clean-machine packaging, or cross-device compatibility;
- broad Vulkan/GPU support beyond the local launch environment;
- implementation of deferred edge authoring, model-backed/autonomous AI, or other future
  workspace capabilities.

The native shell is therefore launch-proven locally, not generally promoted as
a finished desktop product.
