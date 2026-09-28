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
  reversible command. The operation is topology-safe; interactive selection and
  tool UI are not yet shipped.
- `carto_editor` provides ephemeral object, vertex, and face selection with
  replace/add/toggle operations and stale-context validation. Selection is not
  serialized as authoring truth.
- The selected-face extrusion adapter validates selection mode, cardinality,
  and mesh context before delegating to the reversible topology command.
- `ToolRegistry` exposes explicit tool IDs and command factories; successful
  actions are submitted through `CommandBus`, while unknown, duplicate, or
  null-command registrations fail closed. Tool factories receive no mutable
  mesh accessor; the built-in context exposes only approved command builders.
- `carto_editor` routes supported transforms, vertex edits, and mesh-object
  creation through reversible commands, preserving stable IDs across
  undo/redo and keeping failed commands out of history.
- `carto_project` persists authoring state in a versioned, deterministic text
  format with pre-commit validation and atomic replacement. Render caches are
  not serialized as project truth.
- `carto_io` provides a deliberately narrow OBJ importer/exporter. Unsupported
  features are reported as loss rather than silently presented as preserved.
- `carto_render` accepts compiled mesh snapshots only. It is a backend-neutral
  render-scene seam; it is not yet a Vulkan renderer.
- `cartographer_cli` can create/validate a sample project and import/export
  OBJ geometry without a login or network connection.

## Explicitly not implemented yet

The following are contracts and roadmap items, not shipped capability in this
checkout: the native desktop shell, Vulkan resource backend, workspace/pane
system, selection UI, stable edge mode, inset/bevel/edge tools,
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
    -> carto_editor command/transaction boundary
        -> carto_project / carto_scene authoring truth
            -> carto_geometry validation and compilation
                -> carto_render compiled snapshot seam
                    -> future Vulkan/platform backend
```

`carto_render` cannot include editable topology internals. Authoring objects
remain the source of truth; compiled geometry and render state are disposable,
revision-tagged outputs. See [architecture overview](docs/architecture/overview.md)
and the [target map](docs/architecture/target-map.md).

## Governance and provenance

The implementation is a clean-room public reimplementation of generic
capabilities described by the specification. The specification's VANTA
inspection is treated as design evidence, not a license to copy private code.
See [provenance and IP boundary](docs/provenance.md),
[ADR-0001](docs/adr/0001-standalone-authoring-boundaries.md), and the
[capability matrix](docs/capability-matrix.md).
The concrete 0.1 evidence is recorded in the
[acceptance matrix](docs/verification/acceptance-matrix.md).

The code license is intentionally not guessed. A permissive license, CLA/DCO
policy, and trademark policy require the business/counsel decision described in
the specification; until that decision is recorded, this checkout must not be
represented as licensed for redistribution.
