# Scalar anisotropy in the authoring viewport, 2026-11-18

The D3D12 authoring viewport discarded an untextured material's surface packet
when its only advanced effect was scalar anisotropy. The constant shader had no
anisotropy lane, so an authored brushed response could appear isotropic. The
viewport now routes nonzero scalar anisotropy through the surface shader and
uses UV0-derived or authored UV0 tangents, consistent with the native scene
adapter. It explicitly rejects a missing UV0 frame rather than inventing a
direction. When a non-normal texture uses a different UV identity, the mesh
retains that stream independently from the UV0 tangent source. A normal map
using a different UV identity is rejected because the current draw has only
one tangent frame.

Tangent transport follows the scene renderer's shortest-rotation construction
when a mapped normal approaches the original tangent, and retains handedness
for mirrored UVs and mirrored object transforms. A near-zero bent environment
normal falls back to the valid surface normal. The viewport's existing
anisotropic distribution still uses isotropic Schlick masking and an
approximate bent environment normal. It is not the scene renderer's correlated
Smith model or a complete glTF anisotropic IBL implementation. Optical v5
texture maps and a second normal-map tangent frame remain unsupported.

The pre-change GPU executable accepted missing UV0, failing the new explicit
refusal test. On the final candidate, a synthetic native viewport fixture checks
rotation response against an independently calculated direct-light value,
quarter-turned and mirrored UV equivalence, mirrored object handedness, an
explicitly authored tangent, texture sampling from a second UV set, a nearly
tangent normal map, and rejection of a conflicting normal-map UV. Six related
suites passed on the
local NVIDIA Quadro P5200: authoring surface, two-sided, full GPU viewport,
CPU shader compilation, and the separately maintained authored-anisotropy CPU
and GPU reference checks. The last two test the scene kernel, not this
viewport's approximate masking model. The CPU shader check alone is not GPU
execution evidence.

These tests do not establish a physically accurate BRDF, a 4K prop frame-time
or memory budget, temporal stability, cross-engine appearance, or human visual
acceptance. Another VANTA CPU reference render and subsequent VANTA build ran
concurrently, so no timing comparison is inferred from this run.
