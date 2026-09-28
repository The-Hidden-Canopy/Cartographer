# Capability matrix

| Area | 0.1 status | Evidence | Boundary / non-claim |
| --- | --- | --- | --- |
| C++23/CMake core | Implemented | clean configure/build and CTest | no package-manager dependency claim |
| scene identity/hierarchy | Implemented | cycle/dangling/world-transform tests | no animation or collection semantics |
| editable polygon mesh | Implemented | invariant and compile tests | non-manifold input is rejected; no sculpt/retopo/UV/materials |
| primitive construction | Implemented for box and plane | deterministic primitive and bounds tests | no interactive selection UI or parametric history |
| component editing | Implemented for validated vertex positions, including project-bound selected-vertex tool dispatch | reversible vertex command, multi-mesh routing, and topology-rollback tests | no edge tools, inset, bevel, multi-vertex transforms, or grouped transactions |
| face topology editing | Implemented for single-face extrusion | extrusion topology, rollback, and undo/redo tests | no inset, bevel, edge tools, selection UI, or parametric history |
| editor selection state | Implemented for object, vertex, and face modes | mode isolation, mesh-identity/revision context, missing-ID, and stale-selection tests | ephemeral only; no edge identity, UI, or project persistence |
| selection-to-tool dispatch | Implemented for single-face extrusion | wrong-mode, multiple-selection, stale-context, and delegation tests | multi-face tools and interactive UI are deferred |
| tool context routing | Implemented for object-bound multi-mesh face extrusion and selected-vertex editing | selected second asset changes only that asset; undo/redo and project validation pass | mesh identity beyond the owning scene object and broader multi-selection semantics are deferred |
| tool registry | Implemented for registered command factories without mutable mesh access | registration, duplicate-ID, unknown-tool, null-command, multi-mesh routing, and history tests | no plugin loading or UI discovery contract yet |
| half-edge adjacency | Implemented as validated snapshot | boundary/radial tests | not yet an incremental editing kernel or radial non-manifold representation |
| command undo/redo | Implemented for transforms, vertex edits, mesh-object creation, and project-bound mesh tools | failed-command, topology-rollback, ID-preservation, and stale external-edit tests | grouped/coalesced history is deferred |
| application/session boundary | Implemented for the headless 0.1 vertical slice | application tests cover new/open/save, selection, tool dispatch, diagnostics, undo/redo, and immutable viewport snapshots | native window and GPU runtime acceptance remains pending |
| workspace/pane state | Implemented as validated ephemeral application state | application tests reject duplicate, unknown, empty, or viewport-less layouts and accept compact layouts without changing document revision | no persisted user layout or multi-window docking |
| compiled identity mapping | Implemented for source vertices and triangle faces | compiled snapshot and application tests retain stable vertex/face IDs for viewport hit mapping | no GPU picking or edge identity |
| project persistence | Implemented v1 text format | round-trip/future-version/duplicate-record/trailing-data/atomic tests | no chunk store or recovery journal |
| OBJ interchange | Implemented narrow path | import/export, feature-loss, and oversized-face tests | no glTF/STEP/IFC or materials; 128 MiB/record safety limits apply |
| render seam | Implemented headless snapshot | compiled-only, stale-revision, and application snapshot tests | no claim of GPU execution |
| Vulkan runtime probe | Source wired, not runtime-verified | optional target and fail-closed SDK discovery | no device/runtime result on this host |
| desktop shell | Source wired, not runtime-accepted | optional Win32/ImGui/Vulkan target consumes only application actions/snapshots | no native build, launch, DPI, input, or GPU claim |
| private integrations | Intentionally absent | target map/provenance review | no RegOS/IDA/HAVEN/VANTA dependency |
| open-source licensing | Pending owner decision | ADR-0002 | no redistribution license claim |
