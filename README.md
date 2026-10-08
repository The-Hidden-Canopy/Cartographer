# Cartographer

Cartographer is a standalone-first C++23 spatial authoring system for mesh
modeling, technical geometry, and future CAD, architecture, animation, and
interchange workflows.

The implementation is a standalone 0.1 foundation. Its public/private release
boundary is recorded in [docs/provenance.md](docs/provenance.md) and
[docs/public-private-boundary.md](docs/public-private-boundary.md). Moat assets
and not-yet-cleared integration material are maintained outside this public
checkout.

## What works today

- CMake 3.25+ builds narrow, independently linkable C++23 libraries without
  private repositories, network services, AI runtimes, or GPU SDKs.
- `carto_scene` owns stable object IDs, double-precision transforms, explicit
  parentage, deterministic world-transform evaluation, and cycle/dangling
  reference rejection.
- `carto_geometry` owns editable polygon authoring data, stable element IDs,
  half-edge adjacency snapshots, topology validation, deterministic compiled
  triangles, normals, bounds, and source revision stamps.
- `carto_geometry` provides deterministic box and plane primitive constructors;
  validated vertex-position edits are reversible through `carto_editor`.
- A single face can be extruded into side faces and a cap through a validated,
  reversible command. The operation is topology-safe; the headless UI contract
  and optional source shell now route supported selection/tool actions through
  the application boundary, while native visual acceptance remains pending.
- `carto_editor` provides ephemeral object, vertex, edge, and face selection with
  replace/add/toggle operations and stale-context validation. Selection is not
  serialized as authoring truth.
- The selected-face extrusion adapter validates selection mode, cardinality,
  and mesh context before delegating to the reversible topology command.
- `ToolRegistry` exposes explicit tool IDs and command factories; successful
  actions are submitted through `CommandBus`, while unknown, duplicate, or
  null-command registrations fail closed. Tool factories receive no mutable
  mesh accessor; the built-in project-bound context resolves the selected scene
  object to its mesh asset and exposes only approved command builders.
- `carto_editor` routes supported transforms, vertex edits, and mesh-object
  creation through reversible commands, preserving stable IDs across
  undo/redo and keeping failed commands out of history.
- The built-in project tool context also routes a single selected vertex to a
  validated position-edit command; invalid geometry, stale owners, and wrong
  selection modes fail without changing history or sibling assets. Project
  undo/redo uses mesh-revision preconditions and refuses to overwrite an
  intervening external edit.
- `carto_application` provides the headless front-end boundary: new/open/save,
  outliner-style object views, validated workspace/pane actions, selection
  actions, tool dispatch, undo/redo, diagnostics, and immutable viewport
  snapshots. Panels and native shells dispatch actions through this boundary
  rather than mutating the project directly.
- `carto_ui` provides the headless presentation/controller layer:
  centralized tokens, density/theme/operator/workspace state, bounded local
  preferences, command-palette/shortcut routing, operation/problem views, and
  explicit edge-authoring boundaries. It does not own project truth, provide a
  direct mutation path, or include private authoring services.
- `carto_world` provides five bounded provider-neutral authoring primitives:
  semantic areas and paths, structure graphs, reconstructible instance fields,
  and sparse variant layers. Cross-references, hierarchy, finite geometry,
  recipe identity, population bounds, and canonical persistence fail closed;
  runtime materialization and adaptive selection are deliberately absent.
- `carto_project` persists authoring state in a versioned, deterministic text
  format with pre-commit validation and atomic replacement. New saves use v7,
  including optional graph reference, topology lineage, mesh attributes/UVs,
  generic scientific state, and the five-primitive world model; v1-v6 projects
  remain readable. Render caches are not serialized as project truth.
- `carto_scientific` provides typed, provider-neutral regions, fields, studies,
  results, probes, parameters, provenance, a bounded morphology structure graph,
  semantic functional-genome records with deterministic gene/module and
  source/lineage/program-identity diff, explicit neural-value changes, optional
  source-digest/lineage identity, and bounded
  provider-neutral functional-genome exchange envelopes that bind a source
  preservation receipt to a canonical semantic digest, plus stable-ID guarded
  gene replacement/removal for journaled authoring edits,
  developmental-program/trace records with optional tissue assignment and
  parameter-value evidence, plus lineage/mutation/recombination records, field-linked
  morphology growth-domain records, deterministic
  timeline/instruction-impact projections, deterministic source-level
  developmental-program diffs for stage/instruction/module changes, and explicit
  morphometric observations/signatures/comparisons plus deterministic
  bilateral landmark (shared-frame, rigid, scale-normalized, and principal-axis)
  with explicit correspondence maps or a name-bound compatibility path,
  CPU landmark measurements (distance, segment-length, angle, branch-angle,
  ratio, centroid, bounding-dimensions, and deterministic principal axes), and
  signature-comparison previews plus shared-frame explicit surface metadata
  deltas for displacement, normal displacement, curvature, thickness, and
  normal angle
  and sequence-genome
  contigs/variants, bounded anatomy/partonomy mappings with ontology release
  metadata and typed tissue-region records with structure/region bindings,
  stage metadata, and geometry,
  field-visualization
  descriptors with CPU-only scalar sample-to-color, vector glyph, bounded slice-plane,
  marching-squares contour, bounded marching-tetrahedra isosurface, bounded front-to-back
  volume projection, single-probe readout, and bounded 2D streamline previews,
  plus revision-bound Cartesian parameter-sweep previews that expand authored
  study parameters without solver/provider execution,
  provider-import lineage receipts with explicit preservation
  dispositions, and phenotype-capability
  derivations with typed source tracing and derived forward/reverse capability
  queries, plus gene-regulatory and spatial-expression records. It is an authoring substrate, not a solver, sequence-genome
  interpreter, anatomy behavior catalog, runtime, or renderer. Anatomy-provider
  publication bindings retain semantic IDs, laterality, source project revision,
  geometry/license lineage, and local edits without importing VANTA runtime
  authority. A separate authoring-side candidate gate requires matching source
  revision and geometry digests before a provider handoff can be proposed.
  A derived read-only development timeline projection orders stages and exposes
  planned versus observed instruction/structure changes without persisting a
  second source of truth.
  Scientific replay capsules record source revision, provider/solver identity,
  seeds, parameter references, dataset digests, and expected outputs without
  claiming execution. The library is not a MENAGERIE runtime, VANTA runtime,
  or telemetry service.
- `carto_io` provides OBJ, polygon-preserving ASCII PLY, binary/ASCII STL, and
  a bounded glTF export profile. Unsupported features are reported as loss
  rather than silently presented as preserved.
- `carto_render` accepts compiled mesh snapshots only. It is a backend-neutral
  render-scene seam with stable source vertex/face identity mapping and
  validated material, PBR-reference, and render-request contracts; it is still
  not a frame renderer.
- `carto_gpu` provides typed generational resource handles, exact format/usage
  descriptors, device-capability contracts, deferred lifetime retirement, and
  backend-neutral descriptors; `carto_render_graph` validates and compiles
  headless resource hazards without submitting GPU work.
- `carto_assets` provides bounded content-addressed SHA-256 blobs, source-
  fidelity texture metadata, mip validation, and semantic color-space rules, and
  `carto_journal` provides revision-bound append-only records with a verified
  hash chain. These are foundations, not a replacement for the v1 project
  lineage or a crash-recovery claim.
- `ProjectPackage` creates and validates the proposed human-readable package
  manifest/layout and owns the blob root. It intentionally does not create a
  fake `document.db`; SQLite WAL storage remains a separately accepted layer.
- `carto_sdk` exposes a bounded opaque C ABI over the headless application
  session, with explicit version/status/error ownership. It is not yet a full
  multi-language SDK or shared-library distribution.
- `carto_providers` resolves bounded capability descriptors through explicit
  lifecycle states, and `carto_plugin_protocol` defines a sandbox-default,
  length-bounded host envelope. Neither target loads third-party code or grants
  arbitrary project authority yet.
- The optional `carto_vulkan` headless runtime rung and Win32/Dear ImGui desktop
  shell are source-wired behind explicit CMake options. The headless rung
  creates a logical device, obtains a graphics queue, accepts and uploads a
  caller-provided compiled mesh, compiles the acceptance shaders, submits an
  indexed mesh into an RGBA16F/D32 target, and verifies bounded readback when run
  with an explicit Vulkan SDK. The acceptance seam requires Vulkan 1.1, binds
  the compiled mesh to an expected source revision, selects the physical device
  by UUID, and has an opt-in 8192x8192 execution check. The desktop shell now has a
  product-shaped drafting workbench with a fixed work surface, Tool Rack,
  Instrument Bay, movable/resizable instruments, an Operation Ledger,
  project/revision state, diagnostics, settings, explicit unavailable-state
  language, and validated local persistence for rack visibility and instrument
  geometry. A local Debug build and responsive native-window launch have been
  exercised against pinned Vulkan SDK/ImGui prerequisites; native click-through,
  pixel, DPI, resize, OS-level tear-out, and broad GPU compatibility remain
  separate acceptance gates.
- `cartographer_cli` can create/validate a sample project and import/export
  OBJ, PLY, STL, and the public glTF profile without a login or network
  connection.

## Not yet runtime-accepted

The following are contracts and roadmap items, not runtime-accepted capability
in this checkout: production Vulkan PBR/render-graph/submission-lifetime/
presentation lifecycle, native workspace docking/tab stacks, broader edge-edit tools, bevel tools,
modifiers, UVs, full texture decoding/residency, node graphs, curves/NURBS, sculpting, CAD
sketches/constraints, B-Rep/booleans, BIM objects, animation, rigging, physics,
plugins, Python bindings, full glTF/STEP/IFC interchange, and large-scene
streaming.

No Hub service, account authority, model runtime, moat adapter, RegOS service,
IDA runtime, or cloud dependency is required or linked. OWH, GMIB, anatomy,
simulation, and other ordinary integration surfaces may be maintained in the
adjacent companion tree while provenance and licensing are reviewed; they are
not automatically part of the moat or the public runtime dependency graph.

## Build and test

From the repository root:

```powershell
cmake -S . -B build/default -DCMAKE_BUILD_TYPE=Debug `
  -DCARTO_BUILD_TESTS=ON -DCARTO_BUILD_CLI=ON
cmake --build build/default --parallel
ctest --test-dir build/default --output-on-failure
```

The `default` CMake preset contains the same headless configuration:

```powershell
cmake --preset default
cmake --build --preset default
ctest --preset default
```

The optional native front end does not fetch dependencies. A local Vulkan SDK
and a pinned Dear ImGui source tree are required:

```powershell
cmake -S . -B build/vulkan -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DCARTO_ENABLE_VULKAN=ON
cmake -S . -B build/desktop -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DCARTO_ENABLE_VULKAN=ON -DCARTO_BUILD_DESKTOP=ON `
  -DCARTO_IMGUI_ROOT=C:/path/to/pinned/imgui
cmake --build build/desktop --parallel
```

Configuration fails closed when the SDK or the required ImGui Win32/Vulkan
backends are absent. A successful configure/build still does not establish
GPU, DPI, input, or desktop workflow acceptance.

The headless Vulkan acceptance path uses the SDK-selected preset:

```powershell
$env:VULKAN_SDK = "<vulkan-sdk-root>"
cmake --preset vulkan-headless-release
cmake --build --preset vulkan-headless-release --parallel
ctest --preset vulkan-headless-release --output-on-failure
```

That acceptance validates a caller-provided compiled mesh, creates an RGBA16F/D32
target, compiles the acceptance shaders, submits an indexed mesh, copies a
bounded readback, and checks both the clear pixel and a non-clear center pixel.
Set `CARTO_VULKAN_8K=1` before the runtime test to exercise the same native path
at 8192x8192. It does not claim production PBR,
render-graph, submission-lifetime, presentation, or multi-adapter acceptance.

## CLI smoke workflow

```powershell
build\default\apps\cli\cartographer_cli.exe demo sample.carto
build\default\apps\cli\cartographer_cli.exe validate sample.carto
build\default\apps\cli\cartographer_cli.exe scientific-demo scientific-demo.carto
build\default\apps\cli\cartographer_cli.exe scientific-reference-demo reference-morphology.carto
build\default\apps\cli\cartographer_cli.exe inspect-scientific scientific-demo.carto
build\default\apps\cli\cartographer_cli.exe export-sequence-fasta scientific-demo.carto scientific-sequence.fasta
build\default\apps\cli\cartographer_cli.exe import-sequence-fasta scientific-sequence.fasta fixture-assembly sample://fixture imported-sequence.carto
build\default\apps\cli\cartographer_cli.exe export-sequence-vcf scientific-demo.carto scientific-variants.vcf
build\default\apps\cli\cartographer_cli.exe import-sequence-vcf scientific-variants.vcf reference-sequence.carto imported-variants.carto
build\default\apps\cli\cartographer_cli.exe export-sequence-gff3 scientific-demo.carto scientific-annotations.gff3
build\default\apps\cli\cartographer_cli.exe import-sequence-gff3 scientific-annotations.gff3 reference-sequence.carto imported-annotations.carto
build\default\apps\cli\cartographer_cli.exe export-sequence-bed6 scientific-demo.carto scientific-annotations.bed
build\default\apps\cli\cartographer_cli.exe import-sequence-bed6 scientific-annotations.bed reference-sequence.carto imported-bed6.carto
build\default\apps\cli\cartographer_cli.exe export-sequence-gtf scientific-demo.carto scientific-annotations.gtf
build\default\apps\cli\cartographer_cli.exe import-sequence-gtf scientific-annotations.gtf reference-sequence.carto imported-gtf.carto
build\default\apps\cli\cartographer_cli.exe export-lineage-newick scientific-demo.carto lineage.newick
build\default\apps\cli\cartographer_cli.exe import-lineage-newick lineage.newick scientific-demo.carto imported-lineage.carto
build\default\apps\cli\cartographer_cli.exe export-anatomy-ontology-table scientific-demo.carto anatomy-ontology.tsv
build\default\apps\cli\cartographer_cli.exe import-anatomy-ontology-table anatomy-ontology.tsv empty-anatomy.carto imported-anatomy.carto
build\default\apps\cli\cartographer_cli.exe export-landmark-set scientific-demo.carto scientific-landmarks.carto-landmarks
build\default\apps\cli\cartographer_cli.exe import-landmark-set scientific-landmarks.carto-landmarks reference-morphology.carto imported-landmarks.carto
build\default\apps\cli\cartographer_cli.exe export-functional-genome-exchange scientific-demo.carto scientific-genome.carto-genome
build\default\apps\cli\cartographer_cli.exe inspect-functional-genome-exchange scientific-genome.carto-genome
build\default\apps\cli\cartographer_cli.exe edit-functional-genome-exchange scientific-genome.carto-genome 1 0.35 edited-genome.carto-genome
build\default\apps\cli\cartographer_cli.exe import-functional-genome-exchange scientific-genome.carto-genome imported-genome.carto
build\default\apps\cli\cartographer_cli.exe export-anatomy-provider-candidate scientific-demo.carto anatomy-candidate.carto-anatomy
build\default\apps\cli\cartographer_cli.exe inspect-anatomy-provider-candidate anatomy-candidate.carto-anatomy
build\default\apps\cli\cartographer_cli.exe export-obj sample.carto sample.obj
build\default\apps\cli\cartographer_cli.exe import-obj sample.obj roundtrip.carto
build\default\apps\cli\cartographer_cli.exe export-ply sample.carto sample.ply
build\default\apps\cli\cartographer_cli.exe export-stl sample.carto sample.stl
build\default\apps\cli\cartographer_cli.exe export-gltf sample.carto scene.gltf
```

`scientific-demo` creates a populated, deterministic authoring fixture covering
morphology, field-linked growth domains, functional and sequence genome records, development, lineage,
morphometrics, anatomy/tissue regions, field visualization descriptors, import dispositions,
regulation, replay capsules, phenotype trace links, and an authoring-side anatomy
candidate gate. It is a local preview: it does not execute MENAGERIE/VANTA, run a
provider, compile a phenotype, or claim live scientific results. The generated
project and mesh files are local artifacts and are ignored only when they are
placed under ignored build/temp directories. Do not interpret a successful CLI
smoke run as Vulkan, desktop, CAD, provider, or release acceptance.

`scientific-reference-demo` creates the corresponding structure-only morphology
fixture for testing reference-bound landmark imports; it intentionally contains
zero landmark records.

The functional-genome exchange commands provide a bounded provider-neutral
workflow for source-receipt-bound import, inspection, stable-ID value edits,
journaled `.carto` admission, and deterministic re-export. Export refuses projects without
exactly one source receipt and a source digest; these commands do not execute
MENAGERIE or admit VANTA runtime state. CLI edits add local gene-level
authoring provenance rather than overwriting the original source receipt.

The anatomy candidate commands publish a bounded provider-neutral envelope
containing the candidate manifest, source project revision, semantic bindings,
geometry digests, and canonical manifest digest. They require the authoring
candidate gate to pass and do not claim VANTA validation or runtime admission.

The sequence FASTA commands implement a bounded provider-neutral contig
exchange. Export reports that FASTA omits Cartographer metadata and explicit
variants; import requires caller-supplied assembly/sample identity, records the
source digest and per-contig preservation report, and admits the result through
the normal journaled `.carto` transaction before save/reopen verification.

The sequence VCF commands implement a separate bounded variant exchange. VCF
export requires a reference-genome digest, assembly, sample identity, and
reference provenance, emits explicit one-based coordinate metadata, and reports
that full sequence bytes and per-record provenance are not carried. VCF import
requires a matching reference project with sequence bytes and no pre-existing
variants; unsupported sample columns, INFO fields, allele alphabets, stale
reference alleles, and identity mismatches fail closed. The CLI records a
source-digest preservation receipt and commits the result through the normal
journaled transaction before save/reopen verification.

The sequence GFF3 commands implement a separate bounded annotation exchange.
GFF3 export requires a source-genome digest and sequence-backed contigs, emits
an explicit one-based-inclusive envelope while Cartographer stores intervals as
zero-based half-open values, and carries stable feature IDs, source/type,
score, strand, phase, and escaped attributes. Import requires a matching
reference project with no pre-existing features; unsupported directives,
stale contigs, malformed intervals/attributes, duplicate identities, and
non-finite scores fail closed. GFF3 annotations remain annotations: they do
not become functional genes, developmental programs, phenotype capabilities,
or runtime behavior. The CLI records a source-digest preservation receipt and
commits through the normal journaled transaction before save/reopen
verification.

The sequence BED6 commands provide a separate bounded interval exchange.
Cartographer emits stable identity in the BED name, preserves the native
zero-based half-open interval and strand, converts scores to BED's integer
0..1000 scale, and records the source-genome digest, assembly, sample, and
provenance envelope in comments. Import requires a matching reference project
with no pre-existing features. BED6 type/source/phase/attributes are not
invented on import, and BED6 annotations do not become functional genes,
developmental programs, phenotype capabilities, or runtime behavior.

The sequence GTF commands provide a bounded GTF 2.2 exchange over the same
reference-bound feature layer. They preserve the supported nine-column fields,
quoted attributes, stable Cartographer identity, native interval conversion,
and provenance envelope. Imports reject unsupported directives, malformed
quoted attributes or escapes, stale references, duplicate identities, and
pre-existing features. GTF annotations remain annotations and are never
promoted to functional-genome, phenotype, or runtime authority.

The lineage Newick commands provide a bounded rooted-tree exchange. Export
requires exactly one connected single-parent root and emits stable Cartographer
lineage IDs, generation, and optional source identifiers in controlled labels;
reticulation and second-parent graphs fail closed because Newick cannot carry
them. Import requires the explicit Cartographer envelope and caller provenance,
derives deterministic path-based IDs only for generic external labels, rejects
branch lengths/comments/duplicate identities, and journals the admitted graph
through save/reopen verification. Newick omits native genome/morphology
bindings, mutation/recombination receipts, and per-node provenance; it never
becomes phenotype, development, solver, or runtime authority.

The anatomy ontology-table commands provide a bounded semantic-term exchange.
They preserve namespace/accession identity, term hierarchy, labels, categories,
definitions, ontology version, and source release through a strict hex-delimited
table. Import requires caller provenance and either an empty anatomy shell (which
adopts the declared schema) or a matching schema with no pre-existing terms;
duplicate IDs/accessions, cycles, malformed rows, and unsupported headers fail
closed. Parts, tissue regions, relations, geometry, and per-term provenance are
not silently invented, and ontology labels do not become tissue, phenotype, or
VANTA runtime authority.

The landmark-set commands implement a bounded morphology exchange for typed
landmarks. Export binds the set to a digest of the structure graph and carries
stable landmark IDs, optional structure ownership, coordinates, and explicit
coordinate frames. Import requires a matching morphology reference model with
no pre-existing landmark bindings, rejects unsupported metadata, stale
structures, duplicate IDs, non-finite positions, and frame mismatches, then
attaches admitted landmarks through the validated morphology API and records a
source preservation receipt through the normal journaled transaction.

## Architecture

```text
user/tool
    -> carto_application action/snapshot boundary
        -> carto_editor command/transaction boundary
            -> carto_project / carto_scene authoring truth
                    -> carto_geometry validation and compilation
                        -> carto_render compiled snapshot seam
                            -> carto_render_graph / carto_gpu render planning
                            -> optional Vulkan/platform backend

optional native shell -> carto_ui presentation/controller -> carto_application

Recovery and asset lineage remain parallel to rendering:

```text
authoring revision -> carto_journal (append/replay/verify)
immutable bytes    -> carto_assets (SHA-256 blob)
```
```

`carto_ui` owns the presentation boundary consumed by the optional native
shell; it delegates authoring actions to `carto_application`. The application
actions are validated before entering editor history, and snapshots expose
compiled geometry rather than editable topology. `carto_render` cannot include
editable topology internals. Authoring objects remain the source of truth;
compiled geometry and render state are disposable, revision-tagged outputs.
See [architecture overview](docs/architecture/overview.md), the [private service
boundary](docs/architecture/proposal-boundary.md), the [world authoring
boundary](docs/architecture/world-authoring.md), and the [target map](docs/architecture/target-map.md).

## Governance and provenance

The public implementation keeps authoring truth and generic interchange
contracts independent of moat runtimes. See [public provenance](docs/provenance.md),
[the public/private boundary](docs/public-private-boundary.md),
[ADR-0001](docs/adr/0001-standalone-authoring-boundaries.md), and the
[capability matrix](docs/capability-matrix.md).
The concrete 0.1 evidence is recorded in the
[acceptance matrix](docs/verification/acceptance-matrix.md) and the
[heterogeneous execution closure ledger](docs/verification/heterogeneous-execution-economies.md).

Source released in this repository is licensed under the tracked
[Apache License 2.0](LICENSE). That license does not grant rights to moat
assets held outside this repository, and third-party provenance still requires
review before publication. The project follows the
[Open Canopy Contract](OPEN_CANOPY_CONTRACT.md).
