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
- [x] v4 project format with v1/v2/v3 compatibility, explicit meters/right-handed/Y-up contract, optional graph reference, and bounded project-bound topology receipt lineage
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
- [x] stable-ID topology provenance adapter with revision-bound receipt mapping and explicit created/deleted mappings
- [x] workflow benchmark ledger with honest per-domain closure status
- [x] bounded worker-backed evaluation scheduler with unpublished candidate receipts
- [x] vendor-neutral web geometry C1-C5 slice: deterministic leaf clustering, root hierarchy, revision-bound FaceId provenance, bounded runtime-page payloads, package reader/writer, and structural corruption tests
- [x] web geometry evaluation-graph publication and downstream-closed incremental invalidation with cluster/page dirty identities
- [x] web geometry read-only cluster diagnostics snapshots with explicit grouping-only warnings and package-bound conservative error receipts for editor/problem consumers
- [x] web geometry backend-neutral cluster-overlay projection math with explicit matrix conventions, clipping, viewport bounds, and fail-closed non-finite handling
- [x] web geometry deterministic package-bound page-interest planning from visible overlay identities without residency mutation
- [ ] web geometry renderable parent simplification, VANTA import admission, native overlay rendering/pointer integration, and native runtime admission
- [x] project-package graph reference binding, journal/checkpoint preservation, polygon topology-provenance adapter, and v4 project-bound topology receipt persistence
- [x] opt-in Windows D3D12 adapter/resource boundary and native identity smoke acceptance
- [x] bounded D3D12 native acceptance tranche: runtime DXIL suite, offscreen sampled-material PBR into an HDR target, HDR-to-display tone-map execution/readback, shadow-depth execution/readback, temporal resolve over current/history/motion inputs, storage-texture compute dispatch/readback, deferred descriptor/resource lifetime, typed DXGI back-buffer import, resize/present, desktop launch smoke, and 8K render-target clear on the Quadro P5200
- [x] D3D12 native timestamp capture, queue-aware compute shader states, in-flight command-context reuse, and a warmed temporal-kernel stress benchmark with separate GPU/host timing receipts
- [ ] D3D12 production backend with full render-graph parity (PBR/HDR/IBL/shadow/temporal execution, final presentation integration, input/DPI workflow, and GPU performance/8K frame evidence remain)
- [x] SHA-256 content-addressed blob store with atomic publication and integrity verification
- [x] append-only revision-bound journal with hash chain, verification, replay callback, content-addressed checkpoints, and staged transaction publication
- [x] authority-preserving `carto_ui` foundation with design tokens, density/theme/operator/workspace state, operation/problems projections, shortcut routing, and bounded local preferences
- [x] canonical workspace registry with validated availability, future-workspace reasons, and workspace-specific default layouts
- [ ] desktop shell native visual/click-through/DPI/resize acceptance, full docking, OS-level tear-out, clean-machine packaging, and workspace registry integration evidence
- [ ] Vulkan resource backend and viewport (optional source path present; SDK/GPU acceptance pending)
- [x] directory project package layout, human-readable manifest boundary, and optional document-bound graph snapshot reference (SQLite database and migrations remain pending)
- [x] content-addressed evaluation-graph snapshot reference in the package manifest
- [ ] SQLite WAL authoring database, migrations, integrity scanner, and cache separation
- [x] read-only startup recovery inspection plus explicit human recovery of the latest verified snapshot-bearing journal entry
- [ ] startup recovery UI, recovery-versus-explicit-save decision surface, and crash-injection acceptance (snapshot-envelope replay is present)
- [x] bounded opaque C ABI smoke surface with a C compiler/linker fixture
- [x] opt-in shared-library packaging for the bounded C ABI with explicit platform symbol visibility
- [x] bounded ABI range negotiation; C++/Python/C# bindings remain deferred
- [x] capability provider lifecycle/resolve foundation
- [x] sandbox-default bounded plugin envelope foundation with fail-closed
  permission and manifest-capability admission
- [x] host-side neutral plugin result admission bound to request identity,
  bounded object payloads, and optional source revisions
- [x] host-side plugin execution-budget contract with bounded wall/CPU/output
  declarations and result-size enforcement
- [ ] built-in provider migration beyond the OBJ CLI gate, process isolation,
  runtime budget enforcement, and host geometry admission
- [x] stable object/vertex/edge/face selection identity through the editor, application snapshot, headless UI, and source-wired viewport picker (native acceptance pending)
- [ ] persistent topology edit patches, loop/ring modeling operators, edge-edit tools, and broader modeling tool interaction (receipt-aware edge-loop/edge-ring/boundary/fan/region/path traversal, deterministic vertex/edge/face conversion, one-start shortest-path selection, session-local repeat/adjust-last replay, bounded incident-edge vertex slide, vertex patches, two-vertex target-weld, bounded coplanar internal-edge dissolve, compatible triangle-to-quad conversion, convex-face poke operators, bounded numeric/grid-snapping substrates, and revision-bound vertex/edge-midpoint/face-center candidate discovery exist; bevel/center-or-last merge modes/attribute-aware weld/arbitrary dissolve/broader triangle pairing/multi-object selection/loop-cut/knife operators, viewport projection/markers, modal numeric entry, and native input acceptance remain pending)

The optional native source path is intentionally not marked complete by the
presence of a window class or renderer type. The CPU device supplies the
offline semantic acceptance rung, including texture sampling and bounded
lighting/temporal references. The bounded D3D12 path now has passing local
evidence for runtime DXIL production, sampled-material PBR/HDR/tone-map,
shadow-depth, and temporal-resolve graphics/compute execution, descriptor/resource retirement, typed swapchain
import/resize/present, a desktop launch smoke, and an 8192x8192 render-target
clear on the Quadro P5200. Those receipts do not claim full IBL/render-graph
scheduling parity, final output encoding, input/DPI behavior, or 8K frame
throughput.
The existing Vulkan path remains compatibility-only until its independent
acceptance and removal decision are complete.

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
