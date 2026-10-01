# Cartographer public/private split

Cartographer's differentiation is the standalone authoring foundation. The Hub
adds proprietary value around that foundation; it does not become a runtime
requirement for opening, editing, validating, or exporting a project.

| Public Cartographer | Private Hub / integration layer |
| --- | --- |
| `.carto` authoring format and validation | providers, models, prompts, weights, and routing |
| geometry and topology kernels | ranking, optimization, and personalization |
| scene, project, revision, undo/redo, and receipts | private telemetry and workflow-learning stores |
| OBJ, PLY, STL, and glTF interchange | runtime adapters and private schemas |
| render-graph and GPU contracts | proprietary GPU passes and tuning data |
| generic Vulkan/backend acceptance | private catalogs and procedural generators |
| offline UI/workbench foundations | accounts, entitlements, collaboration, and hosted projects |

The public side may expose a typed context and a typed proposal contract. It may
not expose raw mutable authoring pointers or permit a remote service to bypass
local validation.

## Project-file rule

The core `.carto` document remains public and versioned. Optional public
extensions must be namespaced and versioned independently. Hub-specific state
belongs outside the ordinary project file or is represented only by an opaque
reference that has no authority by itself.

The following do not belong in public project files:

- credentials or bearer tokens;
- model prompts, weights, or provider handles;
- private telemetry or workflow history;
- runtime entity/device handles;
- private service URLs or arbitrary commands; and
- hidden mutation or promotion instructions.

## Repository rule

The default build is the public foundation. OWH, GMIB, private simulation,
anatomy ingestion/catalog work, private optimization notes, source
specifications, and internal automation are kept in the adjacent private
companion tree and are not installed or linked by the public build.

Private development can use the same public contracts by configuring an
explicit out-of-tree source root. That path is deliberately not part of the
public presets, release instructions, or runtime dependency graph.
