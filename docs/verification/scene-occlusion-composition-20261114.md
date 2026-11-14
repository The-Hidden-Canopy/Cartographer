# Scene occlusion composition (2026-11-14)

The native saved-scene path now keeps a scalar graph ambient-occlusion output
separate from a material texture's strength. The shader uses
`graph_ao * lerp(1, sampled_ao, map_strength)` with a map and `graph_ao`
without one. A material's map strength alone does not darken an untextured
surface. This is an ambient/IBL occlusion approximation, not a shadow,
transmission, multi-bounce AO, or photorealism claim.

The derived factor is range-checked and bound into request and transformed-mesh
cache identities. Scene capture algorithm v36 and mesh cache v10 identify the
changed behavior; generic material storage is unchanged. A signed bounded value
in an existing non-interpolated per-draw float carries map presence and graph AO.

Validation on the local Windows D3D12 device: clean target build and the full
`cartographer_d3d12_scene_renderer_tests` suite passed (1/1, 747.10 seconds).
The suite covers graph-only values, graph-plus-map preparation and native pixels,
no-map source strength, mapped strength, invalid values, and AO-only cached vs
fresh pixels. The first attempt exposed an unused texture left in the new test
fixture; it was removed before the passing run. No visual, production-scene,
or photoreal acceptance is implied.
