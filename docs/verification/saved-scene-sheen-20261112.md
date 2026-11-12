# Saved-scene direct sheen candidate

Date: 2026-11-12. Based on opacity commit `92fc08e` in the isolated
`codex/saved-scene-opacity-20261112` branch. No shared integration checkout,
exporter, private repository, build lease, or runtime was changed.

This note records the sheen commit. The later
[anisotropy candidate](saved-scene-anisotropy-20261112.md) separately expands
the direct specular path and records its own stricter tangent admission.

## Capability gained

The saved-document adapter lowers the existing scalar graph `sheen` output
into a scene-only derived material factor. Missing sheen remains zero. Every
factor must be finite and in [0, 1], including caller-constructed requests.
Directional, point and spot direct-light paths consume that factor through a
previously unused component of the existing vertex payload. The vertex
host/shader layout, root-constant budget, generic material artifact and durable project
schema do not expand.

This is a bounded base-tinted sheen mixture, not a complete cloth model or
photorealism acceptance. It uses existing surface roughness and sampled linear
base color. The effective weight is `sheen * (1 - metallic)`. The existing
base diffuse and specular lobes receive `1 - effective_weight`; the new sheen
lobe receives `effective_weight`. This attenuates the whole existing base for
fractional metals. Fully metallic materials retain their original base lobe.
Both base and sheen sit beneath the existing clearcoat attenuation; the outer
clearcoat lobe is unchanged.

## Mathematical contract and provenance

The locally authored implementation follows the published Charlie sheen
distribution (Estevez and Kulla, 2017) and Neubelt visibility form. No external
source code or private donor material was copied, and no dependency was added.
For perceptual roughness r, alpha is `max(0.07, r*r)`. The normal distribution
is `(2 + 1/alpha) * (1 - (N.H)^2)^(1/(2*alpha)) / (2*pi)`. Visibility is
`1 / max(1e-5, 4*(N.L + N.V - N.L*N.V))`. The lobe is zero when either light
or view is outside the positive normal hemisphere. Direct radiance multiplies
the BRDF by N.L, existing light attenuation, shadow visibility and radiance
exactly once.

The alpha floor and denominator bound are explicit numerical limits. This
candidate does not establish white-furnace energy conservation, multiscattering
compensation, measured cloth agreement, or accurate grazing-limit behavior.
The separately implemented CPU reference is numerical acceptance source, not
GPU evidence.

## Failure boundaries and identity

- Nonzero sheen with any supplied environment refuses. GGX environment mips
  and diffuse SH are not silently reused as a sheen integral.
- The existing fixed 0.03 ambient approximation is unchanged when no
  environment is supplied. Receipts disclose both that approximation and the
  direct-only sheen boundary.
- Transmission, subsurface scattering, anisotropy, graph normal and
  displacement outputs still fail closed. No fabricated lobe is substituted.
- Existing graph revision/digest pins and typed inputs remain authoritative.
  The shared graph-evaluation cache retains sheen along with standard factors.
- Request algorithm v32 and uploaded-mesh cache v8 bind the exact scene sheen
  factor and material slot. Sheen-only changes cannot reuse an older uploaded
  factor. Texture resources need no additional invalidation because sheen adds
  no new texture inputs.
- Preparation and capture do not mutate project bytes or journal history.
  Existing graph serialization stores the authored scalar; no migration is
  needed. Old implementations still explicitly reject the unsupported output.

## Files changed

- `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`: derived factor, contract
  and rendering algorithm version.
- `libs/d3d12/include/carto/d3d12/scene_sheen.hpp`: bounded CPU math reference.
- `libs/d3d12/src/project_scene_renderer.cpp`: existing graph-output lowering
  and shared-material factor reuse.
- `libs/d3d12/src/scene_renderer.cpp`: admission, identity, cache payload,
  upload and receipt limits.
- `libs/d3d12/shaders/cartographer_scene_pbr.hlsl`: direct sheen for all
  supported analytic lights.
- `tests/d3d12_scene_renderer_tests.cpp`: mathematical, preparation, source,
  persistence, GPU numeric and cache regressions.
- Capability matrix, roadmap and these verification notes: evidence boundaries.

## Validation and pending gates

`git diff --check` passed. Independent source review found no blocking
correctness or compilation-plausibility issue, including the final fractional
metallic and clearcoat assertions. These are static checks only.

Authored numerical assertions cover an analytic roughness-one case,
reciprocity, grazing/zero-roughness finiteness, closed-hemisphere zeros,
metal/zero-weight identity and invalid factor/cosine rejection. Preparation
assertions cover shared material reuse, unrelated material isolation,
save/reopen identity, source immutability, stale graph refusal, request digest
changes, invalid factors and explicit environment refusal.

Authored GPU assertions use an odd-sized target with the center sample on the
camera target. They compare direct directional/point/spot values with the CPU
reference, check half-weight interpolation and full-metal preservation, and
compare cached versus fresh HDR/depth after changing only sheen. Fractional
metallic and nonzero-clearcoat checks target the stated mixture and outer-layer
attenuation. These are expected outcomes, not observed results.

No configure, build, compiler, test, benchmark, GPU execution or native visual
review ran. The integration owner must build the final combined Windows tree,
run `cartographer_d3d12_scene_renderer_tests --graph-sheen-only`, rerun
`--graph-opacity-only`, and run the full scene-renderer suite plus required
repository gates. Shader compilation and visual/energy acceptance remain open.

## Risks, rejected alternatives and rollback

Primary risks are uncompiled HLSL/host integration, unexecuted native numerical
assertions, shared-roughness artistic limitations, direct-only environment
refusal and the unchanged approximate ambient term. The mixture intentionally
changes base response for fractional metallic values.

Rejected: lowering sheen into clearcoat, silently accepting environment sheen,
simulating transmission with opacity, extending durable material formats or
generic GPU ABI before all consumers support the new lobe, importing private
implementation code, or treating source review as runtime acceptance.

Rollback only this follow-on commit, retaining the earlier opacity commit.
The existing source graph is preserved; rollback restores explicit sheen
refusal. Dataset and training progress are unchanged; capability is a
source-only candidate and promotion remains pending.
