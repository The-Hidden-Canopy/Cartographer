# CMake target map

The target graph is intentionally one-way and is checked by the repository
layout and link declarations:

```text
carto_core
  |-- carto_assets      (content-addressed blobs)
  |-- carto_gpu         (backend-neutral handles/lifetime)
  |-- carto_scene
  |-- carto_geometry
  |     `-- carto_render   (compiled_mesh.hpp only)
  |-- carto_render_graph (core + GPU resource planning)
  |-- carto_journal      (revision-bound recovery records)
  |-- carto_providers    (capability/lifecycle registry)
  |-- carto_plugin_protocol (bounded extension frames)
  |-- carto_sdk          (bounded opaque C ABI)
  |-- carto_project       (scene + geometry)
  |-- carto_editor        (scene + core)
  |-- carto_io            (geometry + core)
  |-- carto_application   (project + editor + render snapshot composition)
  `-- cartographer_cli    (headless application composition)
```

The default 0.1 graph remains headless. When explicitly enabled and supplied
with its external prerequisites, the optional extension is:

```text
carto_application       (headless authoring composition)
carto_vulkan             (optional Vulkan runtime probe/backend seam)
cartographer_desktop     (optional application + Vulkan + Dear ImGui shell)
```

Vulkan and desktop configuration are fail-closed when prerequisites are
missing; they are not silently replaced by CPU rendering or a web shell.

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
