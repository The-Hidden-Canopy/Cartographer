# Cartographer workflow benchmark ledger

This ledger is the evidence boundary for the gap-closure docuseries. A
capability or file writer is not a closed professional workflow. Each row must
eventually have a reproducible fixture, exact acceptance steps, a regression
identifier, and a loss/diagnostic report.

## Benchmark projects

| Fixture id | Workflow | Required result | Current state |
| --- | --- | --- | --- |
| `CARTO-BENCH-03-RADIO` | Blender-class hard-surface radio | Boolean speaker cuts, arrayed vents, bevel, mirror, subdivision-ready controls, UV/material assignment, GLB export, reopen, undo/redo | Planned; bevel/boolean/modifier/UV kernels are not yet closed |
| `CARTO-BENCH-03-BUILDING-KIT` | Modular architectural modeling | Measured wall/door/window/stair modules, hierarchy, snapping, repeated elements, UV/material assignment, GLB export | Planned; snapping and modular evaluation workflow are not yet closed |
| `CARTO-BENCH-04-RAILING` | Procedural lookdev | Curve path, spacing/height/profile parameters, editable graph, material assignment, baked export | Structural graph exists; curve/material nodes and bake acceptance remain open |
| `CARTO-BENCH-06-HUMANOID` | Maya-class animation slice | Skeleton, skinning, IK, walk/reach keys, animation layer, retarget, GLB/USD export | Planned; animation/rig/deform modules do not yet exist |
| `CARTO-BENCH-07-CLAMP` | Parametric CAD foundation | Constrained sketch, pad, pocket, holes, revolve/boolean, parameter edit, regeneration, broken-reference diagnostics | Planned; sketch/constraint/B-Rep modules do not yet exist |

## Evidence required per fixture

1. Start from a clean local project and record the source revision.
2. Complete the workflow without another authoring application.
3. Save, close, reopen, and compare the declared source/result digests.
4. Exercise undo/redo and one invalid or stale operation.
5. Export through a declared interchange profile and record preserved,
   converted, approximated, and dropped semantics.
6. Record diagnostics and performance separately from correctness.

## Current Wave 0 result

The repository now contains compile-tested structural contracts for the
evaluation graph, bounded candidate scheduler, stale-result publication gate,
domain-specific attribute cardinality, per-domain topology transfer, polygon
kernel stable-ID provenance, package-level graph snapshot persistence, and
revisioned document/journal/checkpoint graph-reference binding. These
contracts are not a claim that
any benchmark project is complete. No benchmark
executable was run in this pass, and no GPU/runtime evidence is represented
here.
