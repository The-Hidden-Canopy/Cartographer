# UV and materials deep-series closure

This document records the bounded implementation slice from the supplied UV +
materials remediation series. It is an evidence map, not a claim that the
entire series is complete.

## Implemented in this slice

- `attributes::UvSetTable` gives corner-domain `vec2` layers explicit stable
  `UvSetId` identity, independent active-for-editing and active-for-render
  policy, lightmap-candidate metadata, layer validation, and consumer-rebind
  deletion protection.
- The glTF exporter now consumes the active corner-domain render UV set as
  `TEXCOORD_0`. It expands the derived render stream at `(face, vertex)` UV
  seams, validates finite coverage, and reports missing active render UVs per
  mesh instead of flattening authored seams into vertex-domain data.
- The exporter derives and emits glTF `TANGENT` frames for UV-bearing meshes
  whose triangles produce a valid frame. Degenerate authored UV triangles are
  omitted from tangent emission and reported explicitly per export; no tangent
  data is fabricated.
- A validated face-domain `condition.material_region` layer now survives glTF
  export as deterministic mesh extras containing source face IDs and region
  IDs. The exporter does not fabricate a `materials` array; visual material
  realization remains an explicit downstream responsibility.
- The Motel 8 invertebrate authored slice now has a deterministic UV atlas
  pass: the active render UVs for the three referenced meshes use padded,
  non-overlapping face islands with an explicit `tile_policy_version` of 2.
  This is a bounded authored result, not a claim that Cartographer has a full
  interactive unwrap and packing workflow.
- `render::TextureRef` is now the compatibility name for an authored texture
  binding. It carries UV-set identity, sampler identity, UV transform, channel
  mapping, color interpretation, and normal convention. Non-finite transforms,
  zero UV identities, and unknown enum values fail closed.
- The native D3D12 graphics executor accepts a bounded consecutive SRV table
  for graphics pipelines. The existing one-texture form remains valid when
  `sampled_texture_count` is zero; the multi-texture form uses one explicit
  sampler on binding zero and consecutive texture bindings.
- The built-in D3D12 PBR shader binds the five `StandardMaterial` texture
  semantics: base color, metallic/roughness, tangent normal, occlusion, and
  emissive. It also carries tangent plus handedness in the derived vertex
  payload and has a bounded masked-alpha path.
- The root-constant budget is intentionally 224 bytes for this first native
  five-slot pass. Five SRV descriptor tables and one sampler table consume the
  remaining D3D12 root-signature budget; this is why the pass does not pretend
  that a 256-byte root-constant layout is valid.

## Evidence

- `cartographer_uv_material_contract_tests`: UV identity/layer validation,
  active-set switching, consumer-rebind deletion rejection, explicit texture
  binding validation, GPU ABI packing, and glTF `TEXCOORD_0` seam expansion plus
  `TANGENT` emission, face-region extras, and fail-closed rejection of invalid
  face-region IDs.
- `motel_08_invertebrate_uv_atlas_pass.carto`: 660 reopened face islands across
  meshes 8, 9, and 13, with source object/geometry/material-region preservation
  and mesh 10 unchanged.
- `build/headless-release`: 20/20 CTest tests passed.
- `build/d3d12-headless-release`: 21/21 CTest tests passed on the local Quadro
  P5200. The native test compiled the updated HLSL through DXC at runtime and
  executed the five-slot material draw, indexed draw/readback, tone map,
  shadow, temporal, compute, 8K temporal/probe, and hidden swapchain paths with empty
  debug receipts.

## Deliberately still open

The series remains incomplete for interactive UV seam authoring, topology-aware
island extraction, texel-density analysis, UDIM/lightmap workflows, and
topology-aware UV repair. The bounded face-island pass is intentionally not a
replacement for those tools. Tangent generation and derived glTF export now
exist, but hard-normal splitting, topology-aware invalidation, and automated
repair of degenerate authored UV triangles remain open. Texture decode/residency,
MaterialGraph, OpenPBR/MaterialX, baking, IBL lookdev, color management, and
broader authored material realization remain open. The current native pass
uses a bounded five-table descriptor layout rather than a bindless or
streaming material system.
