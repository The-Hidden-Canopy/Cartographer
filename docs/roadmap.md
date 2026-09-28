# Roadmap

This sequence follows the specification's 0.1-to-1.0 staging. A milestone is
not complete because a type or stub exists; it requires a workflow, invariant
tests, persistence coverage, diagnostics, and an honest capability entry.

## 0.1 - current foundation

- [x] CMake target graph and headless build
- [x] double-precision scene objects and cycle-safe hierarchy
- [x] editable polygon mesh with stable IDs and topology validation
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
- [ ] desktop shell and workspace registry (source scaffold present; native acceptance pending)
- [ ] Vulkan resource backend and viewport (optional source path present; SDK/GPU acceptance pending)
- [ ] interactive selection UI, stable edge mode, and broader modeling tool interaction (headless dispatch exists; input acceptance pending)

The optional native source path is intentionally not marked complete by the
presence of a window class or renderer type. It requires a caller-supplied
Vulkan SDK and pinned Dear ImGui checkout, then a real build, launch, input,
resize/DPI, and GPU evidence pass.

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
