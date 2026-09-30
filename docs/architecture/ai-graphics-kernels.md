# AI kernel patterns for graphics

The native IDA work was reviewed for patterns that can improve Cartographer's
future graphics and geometry compute path. The source kernels are not imported:
they are CUDA/BF16 training implementations with a different execution,
precision, and evidence boundary. Cartographer remains Vulkan-first and
backend-neutral at the public seam.

## Patterns worth carrying forward

| AI kernel pattern | Graphics use in Cartographer | Existing boundary |
| --- | --- | --- |
| Explicit shape validation before launch | Reject a mesh/material/viewport compute pass before a dispatch can read the wrong buffer shape | `render_graph::GraphBuilder` validates resources, usages, read-before-write, and barriers |
| Forward-owned dispatch reused by replay/backward | Reuse a culling, clustering, or tessellation plan across related passes instead of rebuilding CPU row lists or re-reading authoring state | `RenderScene` already consumes immutable compiled mesh snapshots; GPU handles are generational |
| Persistent device-resident workspaces | Keep compiled mesh, material, and visibility buffers resident across frames, with explicit retirement | `gpu::DeferredDestructionQueue` and typed resource registries exist; no backend allocation claim yet |
| Source-separated contribution/telemetry buffers | Keep selection IDs, normals, diagnostics, and optimization receipts separate from render color or geometry outputs | Viewport declarations already distinguish color, depth, selection ID, and normal targets |
| Seeded deterministic rounding | Use only for an explicitly versioned quantization/compression pass; never change authoring geometry silently | `.carto` remains float64 authoring truth and the VANTA export rejects unsafe float32 narrowing |
| Backend-neutral model/session contract | Allow an AI suggestion to produce a typed, reviewable derived proposal rather than issuing a graphics mutation | Cartographer has no AI mutation authority; VANTA's inference contract also separates outputs from world mutation admission |

## First graphics candidates

The first useful AI-shaped compute passes are deterministic, non-authoritative
derived work:

1. visibility and instance bucketing;
2. meshlet or cluster candidate generation;
3. normal/tangent/LOD proposal generation;
4. material or scene-layout study buffers;
5. export-time validation and optimization reports.

Each pass should be represented as a normal render-graph compute pass with
declared resources and a versioned operation descriptor. Its output must carry
the source project/mesh revision and be discardable when that revision changes.
An AI proposal can suggest parameters or a study, but committing geometry or
materials remains an explicit authoring command.

## Deliberately not ported

The following were rejected for the current `.carto`/VANTA export slice:

- copying IDA CUDA kernels or private headers into Cartographer;
- pretending BF16/FP4/FP8 arithmetic is a graphics optimization without a
  Vulkan shader, device feature, numerical tolerance, and visual acceptance;
- adding a generic "AI optimized" flag to the glTF export without a receipt;
- moving topology identity or authoring truth into a GPU buffer;
- using a CPU/reference pass as evidence of GPU throughput.

The next implementation tranche is a backend-neutral compute-operation
descriptor plus a Vulkan proof for one derived pass. It should be added only
when the optional Vulkan path has a real device acceptance target; until then,
the existing render graph is the correct honest seam.
