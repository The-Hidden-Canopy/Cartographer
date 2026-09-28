# Capability matrix

| Area | 0.1 status | Evidence | Boundary / non-claim |
| --- | --- | --- | --- |
| C++23/CMake core | Implemented | clean configure/build and CTest | no package-manager dependency claim |
| scene identity/hierarchy | Implemented | cycle/dangling/world-transform tests | no animation or collection semantics |
| editable polygon mesh | Implemented | invariant and compile tests | non-manifold input is rejected; no sculpt/retopo/UV/materials |
| primitive construction | Implemented for box and plane | deterministic primitive and bounds tests | no interactive tool/selection layer or parametric history |
| component editing | Implemented for validated vertex positions | reversible vertex command and topology-rollback test | no edge tools, inset, bevel, or grouped transactions |
| face topology editing | Implemented for single-face extrusion | extrusion topology, rollback, and undo/redo tests | no inset, bevel, edge tools, selection UI, or parametric history |
| editor selection state | Implemented for object, vertex, and face modes | mode isolation, context, missing-ID, and stale-selection tests | ephemeral only; no edge identity, UI, or project persistence |
| selection-to-tool dispatch | Implemented for single-face extrusion | wrong-mode, multiple-selection, stale-context, and delegation tests | multi-face tools and interactive UI are deferred |
| tool registry | Implemented for registered command factories without mutable mesh access | registration, duplicate-ID, unknown-tool, null-command, and history tests | no plugin loading or UI discovery contract yet |
| half-edge adjacency | Implemented as validated snapshot | boundary/radial tests | not yet an incremental editing kernel or radial non-manifold representation |
| command undo/redo | Implemented for transforms, vertex edits, and mesh-object creation | failed-command, topology-rollback, and ID-preservation tests | grouped/coalesced history is deferred |
| project persistence | Implemented v1 text format | round-trip/future-version/atomic tests | no chunk store or recovery journal |
| OBJ interchange | Implemented narrow path | import/export tests and loss report | no glTF/STEP/IFC or materials |
| render seam | Implemented headless snapshot | compiled-only and stale-revision tests | no Vulkan/GPU execution |
| desktop shell | Not implemented | fail-closed CMake option | no UI launch claim |
| private integrations | Intentionally absent | target map/provenance review | no RegOS/IDA/HAVEN/VANTA dependency |
| open-source licensing | Pending owner decision | ADR-0002 | no redistribution license claim |
