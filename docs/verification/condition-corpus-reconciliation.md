# CONDITION corpus reconciliation — local evidence register

Date: 2026-10-18  
Scope: retained Cartographer-side CONDITION fixture artifacts in this checkout.  
Decision: **reconciliation is partial; production acceptance remains open.**

This register identifies the retained blockouts and test harnesses relevant to
the CONDITION corpus request. SHA-256 values below were computed from the exact
files named. The retained materials identify Cartographer as the local fixture
custodian, but do not identify the upstream content owner or provide source
delivery manifests. No external source owner is inferred from filenames.

## Fixture crosswalk

| Family and retained artifact | SHA-256 | Source owner evidence | Cartographer authoring question | Save/reopen and stale-source evidence | Visual/performance record |
|---|---|---|---|---|---|
| Canonical Pine Ridge buildings — `tmp/condition/pine_ridge_canonical_buildings_blockout.carto` | `f1ccb8f0ce8737df0841ab83df417c9ca51e1b2b068ffbcc10335618fe3b31ce` | README describes a local Cartographer blockout; upstream owner is not named. | Can reusable building families replace the nine property proxies while preserving semantic roots, placements, and familiarity-anchor IDs? | The file digest is fixed here. No test receipt ties this exact file to save/reopen and stale-source refusal. | No retained rendered review or measured production performance for this exact scene. Graybox-only status is explicit in its README.
| Environment/weather — `tmp/condition/motel_08_safe_place_world_blockout.carto` | `7588b0893fa8aa15bc8432884bdfc8aeed344a74bfd44ee4abefd2c8860aede3` | README calls it Cartographer-authored blockout geometry; no upstream owner named. | Can the Motel 8 environment retain stable world layout while weather/season variants are source-bound and reviewable? | No exact-file save/reopen and stale-source receipt was found. The separate weather harness has a save/reopen path, but it is not this project file. | No production weather render or performance capture retained.
| Weather/seasonal witness — `tmp/motel_weather_seasonal_pass/motel_08_weather_seasonal_pass.gltf` (harness source: `main.cpp`) | GLTF `e8101903bf4e84476d2227ccf001b7b196b3f539ce3a50efbc5da4cc08c7c47a`; harness `3a827d1082adba8e19f45cb5c19993f8668651b23f63f629b9ecca1a5f012866` | Local generated test artifact; no upstream owner is named. | Do seasonal geometry/material witnesses retain source identity, UV/material payload, and semantic state without implying weather simulation? | Harness code contains save/reopen, validation, and source-preservation assertions. This report did not rerun the harness or establish stale-source refusal for this exact output. | Harness duration is not a performance benchmark; no renderer-backed visual capture retained.
| Topography/elevation — `tmp/condition/pine_ridge_topology_elevation_blockout.carto` and `.condition.json` | CARTO `228ba17e25b4f58e3d21a27ab4f9774b46332e84222abd4bdd48f4b9cd417319`; sidecar `47bbb00059b72a43625787069955b196da446258de2d1e4f70b84e5fe9869544` | README describes a Cartographer blockout; no upstream owner named. | Can elevation bands, drainage lows, road grades, and landmarks be authored as stable source geometry and spatial metadata without claiming final terrain or hydrology? | Exact artifact digests recorded. No exact-file save/reopen and stale-source test receipt found. | README explicitly limits this to proxy wedges/slabs/channels; no final terrain, weather, collision, navigation, or performance evidence.
| Props — `tmp/condition/pine_ridge_misc_prop_library_blockout.carto` | `b45dfb8b8dcbd87e0e5702e0ec1cc53864f30a867cec8f5c3f4dc75cab133c1e` | The file names a Cartographer blockout; no README/source owner was found beside it. | Can a reusable prop library preserve category, scale, placement, and variant identity across Pine Ridge kits? | Digest recorded; no exact-file round-trip or stale-source receipt found. | No retained render review or measured performance record for this exact library.
| Animals — `tmp/condition/pine_ridge_priority_animals_modular_blockout.carto` | `74ba7f7ef6d1c02ac05ab4567809ad0709b8a622b9ae9857912d58783cdeb1cd` | README says local Cartographer authoring data; no external owner named. | Do modular species silhouettes remain distinguishable at the intended scale while component identities survive replacement? | Digest recorded; README gives replacement/identity guidance, but no exact-file round-trip or stale-source receipt was found. | README explicitly says low-detail proxies only; no anatomical, rendered, rigging, or performance acceptance.
| People — `tmp/condition/pine_ridge_population_role_blockout.carto` | `f59a0e37bc65b0b0f26f4c676e3f30952172369669f4768c7c28d2072cb8a5eb` | README says local Cartographer blockout; no upstream owner named. | Can role hooks grow into distinct clothing/body-language variants without losing the shared source rig and role identities? | Digest recorded; no exact-file round-trip or stale-source receipt found. | README excludes final anatomy, materials, animation, and schedules; no visual/performance acceptance retained.
| Vehicles — `tmp/condition/pine_ridge_vehicle_family_blockout.carto` | `d78791b9e3f8b8817b19977eeba62e588da1a8215e5a68d886e7c24d112dcd61` | README says local Cartographer source; no upstream owner named. | Can nine vehicle families preserve recognizable differences and shared-source lineage through damage, motion, material, and ownership variants? | Digest recorded; no exact-file round-trip or stale-source receipt found. | README limits this to low-poly blockout; suspension, animation, collision, weather, final materials, and performance are unverified.
| Visual-completeness family harness — `tmp/motel_visual_completeness_fixture_family_pass/main.cpp` | `25761e8ab195d3ce2c16ac2378d61843c410ac2ddc28814a2c69093bec5f60bd` | Cartographer test harness; no upstream art owner named. | Do linked fixture families cover interior, exterior, vehicle, and portable-object source payloads after reopen? | Harness source asserts save/reopen, source preservation, and document validation. No run receipt or stale-source assertion is established by this reconciliation. | Structural witness only; no retained screenshot or performance benchmark.
| Visual-completeness state witness — `tmp/motel_visual_completeness_state_witness_pass/main.cpp` | `7a2ce760687f120a57b9c6c4759e672ca11e8cf16171383b9ed8f0d77b3cedde` | Cartographer test harness; no upstream art owner named. | Do lighting, weather, and perception state witnesses preserve source meshes and remain auditable after reopen? | Harness source asserts save/reopen, source preservation, and document validation. No run receipt or stale-source assertion is established by this reconciliation. | Structural witness only; no retained screenshot or performance benchmark.
| Visual-completeness scene harness — `tmp/motel_visual_completeness_pass/main.cpp` | `d25734bdd94e3ea9b8378f2946f3f1139a63e2796aec957e02887aabdcbfa4ce` | Cartographer test harness; no upstream art owner named. | Does a broader Motel 8 scene retain power/service/wetness/glass source witnesses and UV/material payloads across reopen? | Harness source asserts save/reopen, source preservation, and document validation. No run receipt or stale-source assertion is established by this reconciliation. | Structural witness only; no retained screenshot or performance benchmark.

## Existing shared evidence and limits

- `docs/verification/condition-source-authoring.md` describes the generic
  CONDITION source, stale-source, save/reopen, and consumer-admission test
  contracts. It explicitly says production visual fidelity, collision/layout
  review, foliage rendering, and performance acceptance remain unverified.
- `docs/verification/acceptance-matrix.md` records structural evidence for
  synthetic Motel 8, General Store, pickup, sign, state-history, and effect
  fixtures. Those receipts do not identify the retained Pine Ridge blockout
  hashes above and are not substituted for exact-file evidence.
- The CONDITION docuseries defines full authoring loops and production fixtures
  (Motel 8, General Store, Rural Road, Forest Edge, Pickup, Church, Safe Place,
  Motel Sign, and Wildlife Proxy-to-Final). The retained proxies cover portions
  of that scope, not the complete Definition of Done.

## Validation performed

- Recomputed SHA-256 for all 13 artifact/source files listed in the crosswalk;
  every digest matched the recorded value.
- No CMake or CTest run was made. This was a read-only reconciliation, and the
  existing fixture harnesses write generated outputs into retained local
  directories; rerunning them without an isolated output root could overwrite
  those workspace artifacts.
- Queue marker validation found 195 valid status markers: 0 not started, 81
  started, and 114 completed (rechecked 2026-10-18).

## Closure gaps

1. A named upstream owner/source manifest is absent for the retained local
   fixtures. Cartographer is only the local custodian; no external attribution
   is invented.
2. Exact retained `.carto` artifacts need reproducible load/save/reopen and
   stale-source receipts bound to the hashes in this register.
3. Visual acceptance needs a retained native/rendered capture reviewed against
   named criteria. Structural GLTF/mesh validation does not prove appearance.
4. Performance acceptance needs a reproducible workload, target hardware and
   build identity, measurement method, and raw result. Test wall time and
   synthetic fixture duration do not satisfy this gate.
5. The visual harnesses and weather pass were inspected as source/output files
   but were not rerun during this reconciliation. Existing workspace artifacts
   were left untouched.

The reconciliation task therefore remains **started**. Required follow-up is
owner/source-manifest confirmation plus non-destructive exact-fixture round-
trip/stale runs and externally reviewed visual/performance evidence. Art-only,
gameplay, and runtime behavior remain with their product owners.
