# Untextured authoring transmission and Fresnel, 2026-11-18

The native D3D12 authoring viewport previously discarded the v5 packet for a
material without textures or a dynamic graph. Its constant shader has no
transmission lane, so nonzero scalar transmission was rendered as if it were
zero. The GPU preparer now retains the surface packet for such materials. The
CPU reference and ordinary untextured opaque fast path are unchanged.

The surface shader previously derived its transmitted Fresnel share from IOR,
while reflection used the separately authored dielectric F0. These can differ
in the existing material contract. The transmitted share now uses the same F0
as reflection; IOR continues to control the refracted environment direction.
This is still an environment-only approximation, not scene-color refraction,
multi-interface transport, or a complete energy-conserving layered BSDF.

The native regression uses a directional synthetic HDR environment and an
untextured material. Before the routing change it failed because transmission
had no effect. After routing alone it failed because a fully reflective
dielectric also transmitted environment energy. On the final code it checks
those boundaries, an F0 control, a fully metallic control, IOR direction,
attenuation and save/reopen. The IOR fixture was strengthened after its first
environment provided too little directional contrast to distinguish the rays.

On a local NVIDIA Quadro P5200, the final focused regression and the full
`cartographer_authoring_gpu_viewport_tests` passed. The related
`cartographer_authoring_surface_tests` and
`cartographer_authoring_shader_compile_tests` also passed. The last compiles
shaders on the CPU and is not GPU execution evidence. These small synthetic
fixtures are not a 4K frame-time or peak-memory benchmark, a prop round trip,
or human visual acceptance. The wider fresh build emitted existing warnings in
unrelated project, asset, import, application, UI and test sources; they were
not changed as part of this repair.

At this change, scalar anisotropy without textures or a dynamic graph still
bypassed the v5 surface shader. The subsequent authoring anisotropy repair is
recorded in `authoring-scalar-anisotropy-20261118.md`. Scene geometry behind
glass remains outside the authoring preview's refraction path, and v5 optical
texture maps remain explicitly unsupported there. Further pipeline integration
and actual cross-engine visual acceptance remain open.
