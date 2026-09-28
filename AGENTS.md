# Cartographer engineering contract

Cartographer is a standalone C++23 spatial authoring system. This repository
must build, test, and run without access to private Hidden Canopy repositories,
credentials, network services, AI runtimes, RegOS, IDA, HAVEN, or VANTA.

## Layer boundaries

```text
carto_core
  -> carto_scene
  -> carto_geometry
       -> carto_render (compiled geometry only)
  -> carto_project
  -> carto_editor
  -> carto_io
  -> cartographer_cli
```

Authoring data is the source of truth. Render meshes, bounds, normals, GPU
resources, caches, and UI state are derived or transient. A renderer must not
own editable topology. A panel or tool must not mutate a document directly;
mutations go through validated document APIs and editor commands.

## Required behavior

- Reject malformed topology, dangling references, hierarchy cycles, stale
  derived results, unsupported versions, and path traversal explicitly.
- Do not silently substitute fallback geometry, live data, trust scores, or
  private-runtime behavior.
- Preserve stable IDs and double-precision authoring values through supported
  save/load paths.
- Keep saves atomic and ensure a failed save leaves the previous valid file
  untouched.
- Keep provenance and capability limitations visible in documentation.
- Do not add private VANTA headers, namespaces, comments, fixtures, or build
  targets to public code. Clean-room reimplementation is the default.

When a future subsystem introduces durable state transitions or audit events,
the mutation owner and event path must be documented before implementation.
The current 0.1 core has no RegOS-style authority or audit ledger; it is a
local modeling application and must not invent one.

