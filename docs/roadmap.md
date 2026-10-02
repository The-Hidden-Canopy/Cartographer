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
- [x] v3 project format with v1/v2 compatibility, explicit meters/right-handed/Y-up contract, and optional graph reference
- [x] OBJ import/export with explicit feature-loss warnings
- [x] bounded ASCII PLY and binary/ASCII STL interchange with explicit feature-loss warnings
- [x] bounded glTF export profile with explicit feature-loss warnings
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
- [x] backend-neutral device contract, semantic command IR, CPU reference execution/raster acceptance, and RGBA32F texture sampling
- [x] deterministic CPU PBR/HDR, shadow-PCF, and temporal-history reference contracts
- [x] GPU preparation ABI, explicit vertex layouts, canonical render pass plan, and packaged native shader sources (execution pending)
- [x] precision-role kernel contracts, local evidence receipts, artifact identity, and explicit fallback boundaries (execution pending)
- [x] evaluation graph identity/DAG validation, deterministic digest, cache key, and stale-publication gate
- [x] bounded versioned evaluation-graph serialization with hostile-input rejection
- [x] domain-specific attribute layers with explicit topology-transfer policy
- [x] stable-ID topology provenance adapter with explicit created/deleted mappings
- [x] workflow benchmark ledger with honest per-domain closure status
- [x] bounded worker-backed evaluation scheduler with unpublished candidate receipts
- [x] project-package graph reference binding, journal/checkpoint preservation, and polygon topology-provenance adapter
- [x] opt-in Windows D3D12 adapter/resource boundary and native identity smoke acceptance
- [ ] D3D12 production backend with headless parity against the CPU command semantics (DXIL, descriptor tables, indexed draw/readback, debug receipts, deferred destruction, and a compile-only Win32/DXGI presentation seam exist; current PSO `E_INVALIDARG` receipt is unresolved)
- [x] SHA-256 content-addressed blob store with atomic publication and integrity verification
- [x] append-only revision-bound journal with hash chain, verification, replay callback, content-addressed checkpoints, and staged transaction publication
- [x] authority-preserving `carto_ui` foundation with design tokens, density/theme/operator/workspace state, operation/problems projections, shortcut routing, and bounded local preferences
- [ ] desktop shell and workspace registry (source consumes `carto_ui`; native acceptance pending)
- [ ] Vulkan resource backend and viewport (optional source path present; SDK/GPU acceptance pending)
- [x] directory project package layout, human-readable manifest boundary, and optional document-bound graph snapshot reference (SQLite database and migrations remain pending)
- [x] content-addressed evaluation-graph snapshot reference in the package manifest
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
presence of a window class or renderer type. The CPU device now supplies the
offline semantic acceptance rung, including texture sampling and bounded
lighting/temporal references; the production Windows path still requires a
passing D3D12 PSO/draw/readback receipt, typed swapchain back-buffer import,
launch, input, resize/DPI, and GPU evidence pass. Native execution is paused
during the current ML ablation. The existing Vulkan path remains
compatibility-only until its independent acceptance and removal decision are
complete.

The cross-repository tranche is also intentionally split. A valid hash chain or
content-addressed blob is not a recovered project, SQLite package, or promotion
receipt until the owning checkpoint replay, database, and user-visible recovery
workflows are independently tested. The staged transaction boundary now carries
an optional graph reference through document, journal, and checkpoint state, but
does not make the package manifest and document database one atomic multi-file
commit.

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
