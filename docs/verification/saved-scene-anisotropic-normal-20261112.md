# Saved-scene anisotropic normal-map candidate

Date: 2026-11-12. Follow-on to isolated commit `912fd4d`, on
`codex/saved-scene-opacity-20261112`. Shared integration work, private sources,
graph exporters, build leases and runtime sessions were not changed.

## Capability and bounded contract

Direct anisotropic highlights may now coexist with an authored normal texture
on the same material. Nonzero anisotropy still requires actual UV0/tangent
payloads. Its normal map must use UV0, an authored rotation of exactly zero,
and positive X/Y UV scales. Both DirectX Y- and OpenGL Y+ conventions, positive
nonuniform scale, offset, pivot, channel mappings and existing normal scale
retain the ordinary scene normal-map path. This does not add a graph-normal
node or normal graph output support.

Rotated/reflected normal-map transforms still refuse for an anisotropic
material: the existing upload reorients its one tangent frame for that map,
which would otherwise silently rotate the authored brushed direction. The
restriction is per material. Other materials on the same object may retain
rotated/reflected UV0 normal maps. The preexisting shared-object UV1 normal-map
conflict still refuses because only one primary tangent stream is available.
Zero anisotropy keeps its previous normal-map admission and shading path.
Even a nonzero rotation equal to a full turn refuses; no approximate identity
or authored-value normalization is inferred.

Environment lighting remains unsupported for nonzero anisotropy. Transmission,
subsurface, displacement and graph-normal output remain unsupported. This is
a direct-light rendering candidate, not measured photorealism or energy
acceptance, geometry displacement, normal-map filtering improvement, or IBL.

## Shading-frame transport

The anisotropic axes follow the shortest rotation from geometric normal Ng
to the mapped shading normal Ns. Let k = cross(Ng, Ns), s = length(k),
c = clamp(dot(Ng, Ns), -1, 1), and a = k/s. For s*s > 1e-12:

`T' = c*T + s*cross(a,T) + (1-c)*a*dot(a,T)`.

Otherwise retain T, then remove numerical residue along Ns and normalize.
Reconstruct B' = handedness * normalize(cross(Ns,T')). At antiparallel normals
the retained tangent selects a half-turn around the authored T axis. That
choice is deterministic but neither a unique shortest rotation nor a promise
of continuity near antiparallel normals. A numerical shader fallback, if
needed, is itself orthogonal to Ns.

Unlike direct projection of T onto Ns, this transport stays meaningful when
Ns approaches T. Unlike the divided `1 + dot(Ng,Ns)` formulation, it avoids
that denominator near antiparallel normals. The implementation runs only for
nonzero anisotropy. The direct directional/point/spot mixture, roughness and
clearcoat semantics from v33 otherwise remain the same. The fixed 0.03 ambient
approximation is unchanged; no multiscattering or shading-normal energy
correction is claimed.

Both normal conventions are preserved by the existing upload/sample path.
The extra handedness sign affects Ns correctly. Its sign in B' does not change
the anisotropic kernel, whose bitangent contributions are squared.

The implementation and synthetic fixtures are locally authored from standard
axis-angle/Rodrigues rotation and the existing local GGX implementation. No
external code, private donor, texture asset or new dependency was imported.

## Identity, persistence and file ownership

Request algorithm v34 distinguishes the new frame behavior. Existing material
source and uploaded mesh cache v9 already bind normal-map transforms,
conventions, normal scale, channel mapping and anisotropy; existing texture
identity binds the actual normal source/mip bytes. No upload layout, root
signature, persistent schema, graph format or generic material ABI changed.
Prepared project bytes and journals remain immutable. No migration is needed.

Changed source:
- `scene_anisotropy.hpp`: validated numerical frame reference.
- `scene_renderer.hpp`: bounded normal-map contract and v34 identity.
- `scene_renderer.cpp`: per-material admission and receipt disclosure.
- `cartographer_scene_pbr.hlsl`: shortest-rotation shading axes.
- `d3d12_scene_renderer_tests.cpp`: source, numerical and native regressions.

Capability matrix, roadmap and the original anisotropy handoff link this
continuation. Opus retains integration ownership. Reconcile algorithm/cache
versions with the final integration branch; do not overwrite newer IDs.

## Authored verification and remaining acceptance

Numerical assertions cover identity, known quarter-turns including Ns=T,
independent positive-hemisphere closed form, inverse transport, tilted and
near/exact-antiparallel cases, orthonormality, handedness and invalid inputs.

Source/preparation assertions cover both normal conventions, positive
nonuniform scale/offset/pivot, immutable preparation and exact save/reopen
identity, shared instances, stale material or texture sources, unsupported
rotations/reflections/non-UV0 bindings, direct-request admission, and the
scope of restrictions to the affected material/shared tangent stream.

Native assertions use actual decoded mip texels rather than ideal normals.
They compare mapped-normal directional and point/spot pixels with the
transported-frame numerical reference, including strongly tilted normals,
both conventions, signed strengths, cached/fresh resources, convention-only
and texture-source-only changes, exact zero-strength restoration and receipt
limitations.

`git diff --check` passed. Independent source review found no blocking issue
in the math, host/shader path, tests or documentation. That is source evidence
only, not compiled or executed acceptance.

No configure, compiler/build, shader compilation, tests, GPU execution,
benchmark or visual review ran. Numerical tolerances are unverified.
Source review and whitespace checks do not establish runtime acceptance.
The integration owner must build the final Windows tree, run
`cartographer_d3d12_scene_renderer_tests --graph-anisotropy-only`, rerun the
opacity/sheen focused checks, then run the full native scene-renderer suite
and README repository gates after integration. No push or merge was performed.

## Risk and rollback

Primary risks are uncompiled host/HLSL integration, unexecuted fixtures and
normal-map quantization/float tolerances. Near-antiparallel orientation is a
documented convention. Arbitrary map rotation/reflection would need a separate
authored anisotropy frame or additional verified transport data, not relaxation
of this gate. Isotropic IBL remains an invalid substitute for anisotropic IBL.

Revert only this follow-on commit to restore the v33 blanket normal-map
refusal, retaining the previous opacity, sheen and direct anisotropy work.
Authored project formats and saved graphs are not rewritten. Training/dataset
progress and runtime/visual acceptance are unchanged.
