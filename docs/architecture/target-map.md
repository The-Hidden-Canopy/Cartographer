# CMake target map

The target graph is intentionally one-way and is checked by the repository
layout and link declarations:

```text
carto_core
  |-- carto_scene
  |-- carto_geometry
  |     `-- carto_render   (compiled_mesh.hpp only)
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

The future target graph from the engineering specification is recorded in the
roadmap. New targets must preserve these constraints:

- geometry builds without renderer or application shell;
- renderer consumes compiled geometry and never includes editable topology;
- project persistence owns authoring state, not caches;
- platform headers stay out of core and geometry;
- tests link the narrowest target that proves the behavior.
