# Saved-scene direct anisotropy candidate

Date: 2026-11-12. Based on sheen commit `8e2f8ea` in the isolated
`codex/saved-scene-opacity-20261112` branch. No shared integration checkout,
private donor, graph exporter, build lease, or runtime was changed.

This records the original v33 candidate. Its blanket normal-map refusal is
superseded by the bounded v34 continuation documented in
[the normal-map handoff](saved-scene-anisotropic-normal-20261112.md).

## Capability gained and source boundary

The existing scalar graph `anisotropy` output now lowers into a scene-only
derived factor. It must be finite and in [-1, 1]. Zero retains the existing
scene specular implementation. Signed nonzero values change directional,
point and spot highlights using the authored UV0 tangent axes. Positive values
broaden the tangent-axis distribution, and negative values swap the axes.

This is explicitly a legacy-to-anisotropic GGX mixture, not a pure anisotropic
model at intermediate strengths. It is a bounded direct-light feature, not
complete brushed-metal or cloth authoring and not photorealism acceptance.
There is no independent orientation map, rotation control, anisotropy texture,
or directional roughness texture.

The saved-scene adapter forces authored UV0 into the requested stream set and
selects it as primary before tangent generation. Missing UV0 fails. A normal
map on another material of the same object cannot select a different shared
tangent stream. Direct scene callers must provide real optional UV/tangent
data, not merely a UV-set identity. Existing artifact validation checks finite,
unit, orthogonal tangent frames and consistent optional attributes.

Nonzero anisotropy with a normal-map binding or any supplied environment
refuses explicitly. No arbitrary normal-map tangent transport or isotropic
environment prefilter is presented as anisotropic integration. Material texture
UV transforms do not rotate the anisotropy frame: its orientation follows raw
authored UV0 tangents. World-transform handling and tangent handedness use the
existing scene path.

## Mathematical contract and provenance

The locally authored equations use the anisotropic Trowbridge-Reitz/GGX
distribution and height-correlated Smith visibility. No external source code,
private implementation, dependency, or shader asset was imported.

Let `s = abs(anisotropy)`, `alpha = max(0.045, roughness*roughness)`, and
`aspect = sqrt(1 - 0.9*s)`. The tangent and bitangent alpha values are
`alpha/aspect` and `alpha*aspect`; negative strength swaps them. For unit half
vector H in the shading frame, distribution is:

`D = 1 / (pi*alphaT*alphaB*((T.H/alphaT)^2 + (B.H/alphaB)^2 + (N.H)^2)^2)`.

Correlated Smith visibility is:

`V = 0.5 / max(1e-5, N.L*length(alphaT*T.V, alphaB*B.V, N.V) + N.V*length(alphaT*T.L, alphaB*B.L, N.L))`.

The anisotropic kernel is zero outside either positive shading hemisphere.
The separately implemented CPU reference accepts only finite unit directions
in the explicit tangent frame. The shader reconstructs its anisotropic tangent
and bitangent around the final shading normal. The host zeroes normal scale
when no normal texture is bound; mapped normals remain unsupported here.

For nonzero strength, the existing specular result is blended with `F*D*V`
using weight s. Both share existing clearcoat attenuation. The legacy branch
is unchanged at exactly zero, avoiding a discontinuous switch from its Schlick
geometry model to correlated Smith. Diffuse, outer clearcoat, the preceding
sheen mixture, light attenuation and shadow factors retain their owners.
The legacy fixed 0.03 ambient approximation is unchanged and disclosed.

The floors, maximum aspect ratio and mixed-kernel definition are deliberate
limits. No white-furnace energy acceptance, multiscattering compensation,
measured appearance agreement, speed, or native visual result is claimed.

## Identity and persistence

The shared graph-factor cache includes anisotropy on both first evaluation
and reuse. Request algorithm v33 and uploaded-mesh cache v9 bind signed
anisotropy to each material slot. Changing only anisotropy therefore changes
request identity and invalidates uploaded factors without requiring a generic
material artifact or texture-resource change.

The final unused component of the existing channel payload carries the scalar
without increasing vertex layout or root constants. Generic material schema,
compiled material artifact, GPU-material ABI and durable graph format are
unchanged. Preparation does not mutate project bytes or journals. Save/reopen
retains the already-supported authored graph output and its source pins.

## Files changed

- `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`: factor contract and v33.
- `libs/d3d12/include/carto/d3d12/scene_anisotropy.hpp`: numerical reference.
- `libs/d3d12/src/project_scene_renderer.cpp`: graph lowering, shared reuse and
  explicit UV0 selection.
- `libs/d3d12/src/scene_renderer.cpp`: admission, identity, cache, upload and
  receipt limitations.
- `libs/d3d12/shaders/cartographer_scene_pbr.hlsl`: both direct-light paths.
- `tests/d3d12_scene_renderer_tests.cpp`: mathematical, source/persistence,
  frame, native numeric and cache regression source.
- Capability matrix, roadmap and verification notes: pending acceptance.

## Authored tests and pending validation

`git diff --check` passed. Independent source review found no blocking issue
in the mathematical implementation, host/shader path, final tests or handoff.
That review is not compiled or executed acceptance evidence.

Numerical assertions cover zero/near-zero continuity, signed axis swapping,
an analytic normal-incidence value, reciprocity, roughness/grazing finiteness,
opposite-hemisphere rejection, and invalid factor/direction admission.

Preparation assertions cover shared-material and unrelated-material behavior,
save/reopen identity, source immutability, UV/tangent absence, incorrect primary
UV identity, signed factor bounds, raw request digest changes, and explicit
normal-map/environment refusal. A separate saved fixture checks that another
material's UV1 normal map conflicts only on the same anisotropic object, while
a normal-mapped unrelated object remains admitted. Native assertions compare centered metallic
pixels with the reference for directional/point/spot light, test tangent-axis
rotation, restore exact zero behavior, and compare cached/fresh pixels after
changing anisotropy alone. These assertions have not executed.

No configure, build, compiler, shader compilation, test, benchmark, GPU
execution, or native visual review ran. After reviewing/integrating the scoped
candidate, the integration owner must build the final Windows tree, run
`cartographer_d3d12_scene_renderer_tests --graph-anisotropy-only`, rerun the
opacity and sheen focused checks, then run the full scene-renderer suite and
required repository gates. Numerical tolerances remain unverified.
Reconcile algorithm/cache version numbers with the current integration branch
rather than replacing any newer identifiers chosen by another owner.

## Risks, rejected alternatives and rollback

Primary risks are uncompiled HLSL/host integration, unexecuted source/native
assertions, and the explicit no-normal-map/no-environment limitation. Scalar
strength controls both aspect ratio and mixture weight; that artistic contract
must remain visible rather than being labeled a different material standard.

Rejected: silently choosing a tangent axis for UV-less geometry, letting an
unrelated texture stream select orientation, jumping geometry models at zero,
using isotropic IBL for anisotropic materials, importing private code, or
claiming source review proves runtime correctness.

Rollback only this follow-on commit, retaining opacity and sheen. Existing
graphs are not rewritten; rollback restores explicit anisotropy refusal.
Dataset/training progress are unchanged. Capability is a source-only candidate;
promotion and visual acceptance remain pending.
