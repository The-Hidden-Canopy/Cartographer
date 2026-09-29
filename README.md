# Cartographer

Cartographer is a standalone-first C++23 spatial authoring system for mesh
modeling, technical geometry, and future CAD, architecture, animation, and
interchange workflows.

The repository follows the attached [Cartographer Engineering
Specification](docs/spec/Cartographer_Engineering_Specification.pdf). The
current implementation is the first useful 0.1 foundation, not a claim to
have implemented all sixty specification subsystems.

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
- `carto_ui` provides the headless VANTA-family presentation/controller layer:
  centralized tokens, density/theme/operator/workspace state, bounded local
  preferences, command-palette/shortcut routing, operation/problem views, and
  explicit unavailable states for edge authoring and AI tools. It does not own
  project truth or provide a direct mutation path.
- `carto_project` persists authoring state in a versioned, deterministic text
  format with pre-commit validation and atomic replacement. Render caches are
  not serialized as project truth.
- `carto_io` provides a deliberately narrow OBJ importer/exporter. Unsupported
  features are reported as loss rather than silently presented as preserved.
- `carto_render` accepts compiled mesh snapshots only. It is a backend-neutral
  render-scene seam with stable source vertex/face identity mapping; it is
  still not a Vulkan renderer.
- `carto_gpu` provides typed generational resource handles, deferred lifetime
  retirement, and backend-neutral descriptors; `carto_render_graph` validates
  and compiles headless resource hazards without submitting GPU work.
- `carto_assets` provides bounded content-addressed SHA-256 blobs, and
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
- The optional `carto_vulkan` runtime probe and Win32/Dear ImGui desktop shell
  are source-wired behind explicit CMake options. They remain runtime-
  unaccepted until built against a pinned Vulkan SDK/ImGui checkout and
  exercised on a compatible GPU.
- `cartographer_cli` can create/validate a sample project and import/export
  OBJ geometry without a login or network connection.

## Not yet runtime-accepted

The following are contracts and roadmap items, not runtime-accepted capability
in this checkout: native desktop launch on a real Vulkan SDK/GPU, the complete
Vulkan resource lifecycle, native workspace restore/docking, edge-edit tools, inset/bevel tools,
modifiers, UVs, materials, textures, node graphs, curves/NURBS, sculpting, CAD
sketches/constraints, B-Rep/booleans, BIM objects, animation, rigging, physics,
plugins, Python bindings, glTF/STEP/IFC interchange, and large-scene streaming.

No VANTA target, VANTA header, RegOS service, IDA runtime, HAVEN component,
defense application, or cloud dependency is required or linked.

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

## CLI smoke workflow

```powershell
build\default\apps\cli\cartographer_cli.exe demo sample.carto
build\default\apps\cli\cartographer_cli.exe validate sample.carto
build\default\apps\cli\cartographer_cli.exe export-obj sample.carto sample.obj
build\default\apps\cli\cartographer_cli.exe import-obj sample.obj roundtrip.carto
```

The generated project and OBJ files are local artifacts and are ignored only
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
See [architecture overview](docs/architecture/overview.md) and the [target
map](docs/architecture/target-map.md).

## Governance and provenance

The implementation is a clean-room public reimplementation of generic
capabilities described by the specification. The specification's VANTA
inspection is treated as design evidence, not a license to copy private code.
See [provenance and IP boundary](docs/provenance.md),
[ADR-0001](docs/adr/0001-standalone-authoring-boundaries.md), and the
[capability matrix](docs/capability-matrix.md).
The concrete 0.1 evidence is recorded in the
[acceptance matrix](docs/verification/acceptance-matrix.md).

The code is released under the Apache License, Version 2.0; see
[LICENSE](LICENSE). The project follows the
[Open Canopy Contract](OPEN_CANOPY_CONTRACT.md).
