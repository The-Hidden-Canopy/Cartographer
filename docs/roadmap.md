# Roadmap

This sequence follows the specification's 0.1-to-1.0 staging. A milestone is
not complete because a type or stub exists; it requires a workflow, invariant
tests, persistence coverage, diagnostics, and an honest capability entry.

## 0.1 - current foundation

- [x] CMake target graph and headless build
- [x] double-precision scene objects and cycle-safe hierarchy
- [x] editable polygon mesh with stable IDs and topology validation
- [x] persistent EdgeId/HalfEdgeId/CornerId records with deterministic face-boundary traversal
- [x] revision-bound atomic vertex MeshPatch with inverse generation
- [x] deterministic compiled mesh, normals, bounds, and source revisions
- [x] reversible transform command and history boundary
- [x] versioned project format with atomic save/load
- [x] OBJ import/export with explicit feature-loss warnings
- [x] deterministic box/plane primitives and reversible primitive/vertex editing commands
- [x] atomic single-face extrusion with reversible topology history
- [x] ephemeral object/vertex/face selection with context validation
- [x] validated single-face selection-to-tool adapter
- [x] explicit tool registry dispatching commands through editor history
- [x] project-bound tool context routing selected components to the owning mesh asset
- [x] project-bound selected-vertex position editing with topology-safe rollback
- [x] headless application/session boundary for the first authoring vertical slice
- [x] validated ephemeral workspace/pane state with a required viewport
- [x] stable compiled vertex/face identity mapping for future viewport picking
- [x] backend-neutral generational GPU handles, deferred submission retirement, render-graph validation, and offscreen viewport planning
- [x] SHA-256 content-addressed blob store with atomic publication and integrity verification
- [x] append-only revision-bound journal with hash chain, verification, replay callback, content-addressed checkpoints, and staged transaction publication
- [x] authority-preserving `carto_ui` foundation with VANTA tokens, density/theme/operator/workspace state, operation/problems projections, shortcut routing, and bounded local preferences
- [ ] desktop shell and workspace registry (source consumes `carto_ui`; native acceptance pending)
- [ ] Vulkan resource backend and viewport (optional source path present; SDK/GPU acceptance pending)
- [x] directory project package layout and human-readable manifest boundary (SQLite database and migrations remain pending)
- [ ] SQLite WAL authoring database, migrations, integrity scanner, and cache separation
- [ ] startup recovery, replay inspection, and recovery-versus-explicit-save workflow (snapshot-envelope replay is present; recovery UI and explicit-save decision flow remain pending)
- [x] bounded opaque C ABI smoke surface with a C compiler/linker fixture
- [ ] ABI negotiation, shared-library packaging, and C++/Python/C# bindings
- [x] capability provider lifecycle/resolve foundation
- [x] sandbox-default bounded plugin envelope foundation with fail-closed
  permission and manifest-capability admission
- [ ] built-in provider migration beyond the OBJ CLI gate, process isolation, resource budgets, and neutral-result admission
- [x] stable object/vertex/edge/face selection identity through the editor, application snapshot, headless UI, and source-wired viewport picker (native acceptance pending)
- [ ] persistent topology edit patches, loop/ring traversal, edge-edit tools, and broader modeling tool interaction (identity/traversal and vertex patches exist; topology operators and native input acceptance pending)

The optional native source path is intentionally not marked complete by the
presence of a window class or renderer type. It requires a caller-supplied
Vulkan SDK and pinned Dear ImGui checkout, then a real build, launch, input,
resize/DPI, and GPU evidence pass.

The cross-repository tranche is also intentionally split. A valid hash chain or
content-addressed blob is not a recovered project, SQLite package, or promotion
receipt until the owning checkpoint replay, database, and user-visible recovery
workflows are independently tested. The staged transaction boundary closes the
document-to-journal publication risk but does not implement those later flows.

## 0.2 - non-destructive DCC maturity

Modifiers, dependency evaluation, UV layers, materials, textures, node
materials, and the first real viewport workflow.

## 0.3 - animation foundation

Timeline, curves, basic rigs, skinning, deformers, and revision-aware
animation evaluation.

## 0.4 - CAD sketching

Units, precision input, sketches, constraints, solver diagnostics, and
parametric feature history.

## 0.5 - solid modeling

B-Rep interfaces, tessellation, booleans, and failure-visible solid workflows.

## 0.6-0.9 - platform and integration depth

Cross-platform shell, plugins, scripting, larger-scene streaming, neutral
interchange, and optional Unreal/Unity validation adapters.

## 1.0 - release gate

Stable project format/SDK, clean offline install, migration and crash recovery
fixtures, end-to-end modeling workflow, import/export loss reports, dependency
license audit, and no critical unresolved data-corruption risk.
