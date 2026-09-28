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
  `-- cartographer_cli    (application composition)
```

The 0.1 repository deliberately does not create optional targets for Vulkan,
desktop windowing, Python, Unreal, Unity, STEP, or IFC. The CMake options are
fail-closed when requested rather than pretending a missing backend is
available.

The future target graph from the engineering specification is recorded in the
roadmap. New targets must preserve these constraints:

- geometry builds without renderer or application shell;
- renderer consumes compiled geometry and never includes editable topology;
- project persistence owns authoring state, not caches;
- platform headers stay out of core and geometry;
- tests link the narrowest target that proves the behavior.

