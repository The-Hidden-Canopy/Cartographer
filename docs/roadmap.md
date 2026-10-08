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
- [x] v6 project format with v1/v2/v3/v4/v5 compatibility, explicit meters/right-handed/Y-up contract, optional graph reference, bounded project-bound topology receipt lineage, generic scientific model records, and validated per-mesh attribute/UV persistence
- [x] v7 project format with v1-v6 readability, five provider-neutral world primitives, deterministic nested persistence, journaled replacement, and dangling authoring-binding rejection
- [x] OBJ import/export with explicit feature-loss warnings
- [x] bounded ASCII PLY and binary/ASCII STL interchange with explicit feature-loss warnings
- [x] bounded glTF export profile with explicit feature-loss warnings
- [x] bounded uppercase-IUPAC FASTA sequence-genome exchange with explicit metadata/variant loss reporting and journaled CLI import
- [x] bounded VCF variant exchange with reference-digest, assembly, sample, one-based-coordinate, and provenance gates plus journaled CLI import
- [x] bounded GFF3 sequence-annotation exchange with reference-digest, explicit coordinate conversion, stable feature identity, escaped attributes, and journaled CLI import
- [x] bounded BED6 sequence-annotation exchange with reference-digest, native interval preservation, stable name identity, explicit score conversion, and journaled CLI import
- [x] bounded GTF 2.2 sequence-annotation exchange with reference-digest, quoted-attribute validation, explicit coordinate conversion, stable feature identity, and journaled CLI import
- [x] bounded morphology landmark-set exchange with structure-graph digest binding, stable ownership/coordinate frames, provenance gates, and journaled CLI import
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
- [x] GPU preparation ABI, explicit vertex layouts, and packaged native shader sources
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
- [ ] private algorithmic representation and continuity work remains outside the public build boundary and requires separate release review
- [ ] next-level polygon representation inheritance, backend-neutral identity-buffer planning, temporal-history consumers, view/material/constraint/fracture policy execution, and VANTA/runtime admission
- [ ] web geometry VANTA import admission, native overlay rendering/pointer integration, and native runtime admission
- [x] project-package graph reference binding, journal/checkpoint preservation, polygon topology-provenance adapter, v5 scientific-model and v5 project-bound topology receipt persistence
- [x] opt-in Windows D3D12 adapter/resource boundary and native identity smoke acceptance
- [x] bounded D3D12 native acceptance tranche: runtime DXIL suite, offscreen sampled-material PBR into an HDR target, HDR-to-display tone-map execution/readback, shadow-depth execution/readback, temporal resolve over current/history/motion inputs, storage-texture compute dispatch/readback, deferred descriptor/resource lifetime, typed DXGI back-buffer import, resize/present, desktop launch smoke, and an 8K temporal dispatch with bounded probe readback on the Quadro P5200
- [ ] private execution optimization and measured benchmark work remains outside the public repository
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
- [x] generic scientific substrate with typed entity IDs, regions, boundaries,
  interfaces, fields, studies, results, parameters, probes, provenance,
  deterministic serialization, and journaled `.carto` admission
- [x] bounded morphology structure/attachment/landmark/growth-domain graph, with
  field-linked direction/rate/anisotropy/stage bounds, and semantic
  functional-genome records with deterministic gene/module/neural-value and
  source/lineage/program-identity diff, optional source-digest/lineage identity,
  and persistence; cross-domain acceptance fixtures cover human-organ,
  plant-root, insect-limb, and vascular-tree archetypes without domain-specific
  runtime semantics
- [x] bounded developmental stages/instructions, evidence-bearing traces with
  optional tissue assignment and parameter-value evidence, and generation-checked
  lineage/mutation/recombination receipts with cross-reference
  validation, including a 100-generation mutation-lineage acceptance fixture, plus
  deterministic planned-vs-observed timeline, instruction-impact projections,
  and source-level developmental-program diffs for stage/instruction/module changes
- [x] bounded morphometric observations, shape signatures, and explicit
  structure-to-structure comparisons with finite/range/reference validation,
  plus deterministic fail-closed bilateral landmark, explicit correspondence,
  rigid/scale-normalized/principal-axis alignment, signature-comparison previews,
  and CPU-only landmark measurement previews for distance, segment length, arc
  length, angle, branch angle, ratio, planar polygon area, explicit tetrahedron
  volume, centroid, bounding dimensions, and deterministic principal axes with
  explicit ownership/frame/cardinality validation, planarity/non-degeneracy
  rejection, eigenvalue-gap rejection, and zero-denominator rejection; explicit
  shared-frame surface metadata deltas report displacement, normal displacement,
  curvature, thickness, and normal-angle differences without inferring surfaces;
  explicit lineage-to-signature bindings now produce a deterministic bounded
  CPU evolutionary-morphospace projection with raw/unit-box normalization and
  parent trajectories, including recombination parents, without statistical
  fitting or provider execution
- [x] bounded revision-bound Cartesian parameter-sweep previews with canonical
  axis ordering, authored-bound enforcement, fixed-parameter carry-through,
  deterministic case expansion, and case-count safety caps; sweep planning
  remains separate from provider/solver execution
- [x] bounded sequence-genome contigs and explicit variants with uppercase IUPAC,
  coordinate/reference matching, digest, and provenance validation
- [x] bounded provider-neutral FASTA, VCF, GFF3, BED6, GTF 2.2, and rooted
  single-parent Newick exchange surfaces with explicit coordinate/identity
  envelopes, preservation dispositions, strict fail-closed parsing, and
  journaled CLI admission; Newick rejects reticulation and does not replace
  native lineage identity
- [x] bounded anatomy ontology-table exchange preserving namespace/accession,
  term hierarchy, labels, categories, definitions, ontology version, and source
  release with strict schema/provenance gates and journaled CLI admission;
  ontology terms remain semantic authoring data
- [x] bounded anatomy terms, tissue/organ categories, structure-bound parts,
  ontology version/source-release metadata, typed tissue regions with
  stage/developmental/physical metadata and geometry bindings, partonomy, and
  typed relations with cycle/reference validation
- [x] bounded anatomy-provider publication bindings with semantic IDs,
  laterality, source project revision, geometry/license lineage, local edits,
  typed part/structure validation, and an explicit source/digest-bound
  authoring candidate gate plus a deterministic provider-neutral candidate
  envelope and CLI export/inspect path
- [x] bounded scientific replay capsules with source revision, provider/solver,
  seeds, parameter references, external dataset digests, expected output
  digests, and explicit provenance
- [x] bounded field-visualization descriptors with finite display bounds,
  palette/stage intent, field/region reference validation, and derived
  CPU-only scalar sample-to-color, vector glyph, slice-plane,
  marching-squares contour, bounded marching-tetrahedra isosurface,
  bounded front-to-back volume projection, single-probe readout, and bounded
  2D streamline previews
- [x] bounded provider-import lineage receipts with source/provider identity,
  admitted/considered counts, warnings, feature-loss notes, explicit
  preserved/converted/ignored/unsupported/ambiguous/repaired dispositions,
  v1-compatible nested decoding, and provenance
- [x] provider-neutral functional-genome exchange envelope binding one source
  preservation receipt to a canonical semantic digest, with fail-closed
  tamper and sequence-genome substitution checks, plus stable-ID guarded gene
  replacement/removal for re-exported authoring edits and journaled CLI
  import/inspect/edit/export workflow
- [x] bounded phenotype-capability derivations with forward/reverse typed links
  to morphology, genome, developmental instructions, and development traces,
  plus derived forward/reverse trace queries
- [x] bounded gene-regulatory edges and spatial-expression records with typed
  gene, stage, structure, region, and field targets
- [ ] provider execution, rendered field visualization, and MENAGERIE / VANTA
  execution handoff
- [x] stable object/vertex/edge/face selection identity through the editor, application snapshot, headless UI, and source-wired viewport picker (native acceptance pending)
- [ ] persistent topology edit patches, loop/ring modeling operators, edge-edit tools, and broader modeling tool interaction (receipt-aware edge-loop/edge-ring/boundary/fan/region/path traversal, deterministic vertex/edge/face conversion, one-start shortest-path selection, session-local repeat/adjust-last replay, bounded incident-edge vertex slide, vertex patches, two-vertex target-weld, bounded coplanar internal-edge dissolve, bounded single-segment internal-edge and valence-three vertex bevels, compatible triangle-to-quad conversion, bounded arithmetic numeric/grid-snapping substrates, and revision-bound vertex/edge-midpoint/face-center candidate discovery exist; multi-segment/full bevel policy, center-or-last merge modes/attribute-aware weld/arbitrary dissolve/broader triangle pairing/multi-object selection/loop-cut/knife operators, viewport projection/markers, axis locks, modal numeric confirmation, and native input acceptance remain pending)

The optional native source path is intentionally not marked complete by the
presence of a window class or renderer type. The CPU device supplies the
offline semantic acceptance rung, including texture sampling and bounded
lighting/temporal references. The bounded D3D12 path now has passing local
evidence for runtime DXIL production, sampled-material PBR/HDR/tone-map,
shadow-depth, and temporal-resolve graphics/compute execution, descriptor/resource retirement, typed swapchain
import/resize/present, a desktop launch smoke, and an 8192x8192 temporal
dispatch with bounded RGBA32F probe readback on the Quadro P5200. Those receipts
do not claim full IBL/render-graph scheduling parity, final output encoding,
input/DPI behavior, or full 8K presented-frame throughput.
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
