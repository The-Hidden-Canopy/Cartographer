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
  explicit edge-authoring boundaries and the native AI proposal workflow. It
  does not own project truth or provide a direct mutation path.
- `carto_ai` provides a native, provider-neutral operation ontology and a
  deterministic first planner for bounded face/vertex intents. AI output is a
  typed, revision-bound proposal; safe native proposals are auto-approved by
  default through the same validated preview/apply path, while review mode
  remains available. There is no Python, model runtime, network service, or
  outbound telemetry in this path.
- `carto_project` persists authoring state in a versioned, deterministic text
  format with pre-commit validation and atomic replacement. New saves use the
  v3 authoring envelope and optional graph reference while v1/v2 projects remain readable. Render caches are
  not serialized as project truth.
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
  serializer or a crash-recovery claim.
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
build\default\apps\cli\cartographer_cli.exe export-obj sample.carto sample.obj
build\default\apps\cli\cartographer_cli.exe import-obj sample.obj roundtrip.carto
build\default\apps\cli\cartographer_cli.exe export-ply sample.carto sample.ply
build\default\apps\cli\cartographer_cli.exe export-stl sample.carto sample.stl
build\default\apps\cli\cartographer_cli.exe export-gltf sample.carto scene.gltf
```

The generated project and mesh files are local artifacts and are ignored only
when they are placed under ignored build/temp directories. Do not interpret a
successful CLI smoke run as Vulkan, desktop, CAD, or release acceptance.

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

Drafting Board intent -> carto_ai ontology/context -> typed proposal
    -> carto_editor preview -> policy-authorized auto-apply or explicit commit

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
See [architecture overview](docs/architecture/overview.md), the [public proposal
boundary](docs/architecture/proposal-boundary.md), and the [target map](docs/architecture/target-map.md).

## Governance and provenance

The public implementation keeps authoring truth and generic interchange
contracts independent of moat runtimes. See [public provenance](docs/provenance.md),
[the public/private boundary](docs/public-private-boundary.md),
[ADR-0001](docs/adr/0001-standalone-authoring-boundaries.md), and the
[capability matrix](docs/capability-matrix.md).
The concrete 0.1 evidence is recorded in the
[acceptance matrix](docs/verification/acceptance-matrix.md).

Source released in this repository is licensed under the tracked
[Apache License 2.0](LICENSE). That license does not grant rights to moat
assets held outside this repository, and third-party provenance still requires
review before publication. The project follows the
[Open Canopy Contract](OPEN_CANOPY_CONTRACT.md).
