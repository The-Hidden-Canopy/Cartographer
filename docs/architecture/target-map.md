# CMake target map

The target graph is intentionally one-way and is checked by the repository
layout and link declarations:

```text
carto_core
  |-- carto_assets      (content-addressed blobs)
  |-- carto_attributes  (typed domain layers and transfer policy)
  |     `-- carto_mesh_attributes (polygon-kernel stable-ID provenance adapter)
  |-- carto_eval        (revision-bound graph, cache, scheduler, and snapshot format)
  |-- carto_gpu         (backend-neutral handles/lifetime)
  |-- carto_device_ir   (backend-neutral command stream)
  |-- carto_device      (device/shader/queue contract)
  |     `-- carto_device_cpu (reference execution)
  |-- carto_render      (compiled geometry + render graph + kernel intents)
  |-- carto_scene
  |-- carto_geometry
  |     `-- consumed by carto_render
  |-- carto_render_graph (core + GPU resource planning)
  |-- carto_journal      (revision-bound recovery records)
  |-- carto_providers    (capability/lifecycle registry)
  |-- carto_plugin_protocol (bounded extension frames)
  |-- carto_sdk          (bounded opaque C ABI)
  |-- carto_project       (scene + geometry + package graph snapshots)
  |-- carto_editor        (scene + core)
  |-- carto_io            (geometry + core)
  |-- carto_application   (project + editor + render snapshot composition)
  |-- carto_ui            (presentation state + validated application routing)
  `-- cartographer_cli    (headless application composition)
```

The default 0.1 graph remains headless. When explicitly enabled and supplied
with its external prerequisites, the optional extension is:

```text
carto_application       (headless authoring composition)
carto_ui                 (headless public presentation/controller)
carto_vulkan             (optional Vulkan runtime probe/backend seam)
carto_d3d12              (optional Windows D3D12 device/resource/command/swapchain seam)
cartographer_d3d12_desktop (optional native Win32/D3D12 presentation shell)
cartographer_desktop     (optional application + Vulkan + Dear ImGui shell)
```

Vulkan, D3D12, and desktop configuration are fail-closed when prerequisites
are missing; they are not silently replaced by CPU rendering or a web shell.
The D3D12 target implements native queue, resource, transition, copy, clear,
runtime DXC, graphics-pipeline, draw, texture-readback, debug-receipt,
device-loss, deferred-destruction, sampled-texture descriptor, and indexed
draw/readback paths. The optional Win32/DXGI swapchain and desktop shell
configure and link as a native compile target. Its end-to-end graphics
acceptance is currently blocked at PSO creation with an `E_INVALIDARG` receipt,
and native execution is paused while the host is used for an ML ablation.
Swapchain runtime, back-buffer import, desktop launch, and 8K hardware
acceptance remain unverified.

The remaining future target graph from the engineering specification is
recorded in the roadmap. New targets must preserve these constraints:

- geometry builds without renderer or application shell;
- renderer consumes compiled geometry and never includes editable topology;
- project persistence owns authoring state, not caches;
- blob storage owns immutable bytes, not project schema truth;
- journal records are durable provenance, not the undo stack or a user save;
- blob and journal primitives verify content and lineage before returning data;
- platform headers stay out of core and geometry;
- tests link the narrowest target that proves the behavior.
