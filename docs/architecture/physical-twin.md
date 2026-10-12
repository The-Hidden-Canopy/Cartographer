# Physical twin records

The physical-twin tranche (docuseries ch.04) connects authored geometry to
VANTA physical materials and construction without moving game authority into
Cartographer. `carto_physical` implements the Cartographer side: neutral,
versioned, digest-bound, serialization-ready records only. VANTA owns runtime
embodiment and the physical-material catalog; games own domain truth and
current condition state.

## Authority split

- **Cartographer** (`carto::physical`) owns geometry evidence, construction
  intent, derived mass facts, the surface ledger, and acceptance receipts.
- **VANTA** resolves catalog identities into native physical materials and
  realizes them at runtime. Cartographer records catalog *identity* in
  `PhysicalMaterialIntent` and never copies catalog entries: no display names,
  no resolved constants, no handles.
- **Games** own wear and condition. `SurfaceConditionState` is explicitly
  game-owned (`kGameOwned`), may travel beside a twin record for adapter
  convenience, and is structurally incapable of entering a canonical document
  digest: `PhysicalTwinRecord::serialize()` has no section for it.

## Records

- `GeometryEvidence` — derived once from a `geometry::CompiledMesh` via
  `derive_geometry_evidence`: signed/enclosed volume, surface area,
  per-triangle thickness distribution about the bounds center, volume
  centroid, and the unit-density inertia tensor about that centroid. The
  canonical digest binds mesh identity + source revision + numbers. Open or
  inconsistently wound meshes fail derivation; nothing is substituted.
- `PhysicalMaterialIntent` — stable VANTA catalog identity plus authored
  qualifiers, notably the optional authored density used by mass derivation.
  When no density is authored, mass derivation fails closed instead of
  inventing a catalog constant.
- `ConstructionProfile` — solid / shell / layered / filled. Shells and fills
  carry wall-thickness declarations with provenance (`authored` or
  `derived_from_geometry`); layered constructions bind a copy of the surface
  `MaterialStratigraphy` and validate layer-by-layer correspondence.
- `MassDerivationReceipt` — deterministic CPU derivation (total mass, center
  of mass, inertia tensor) binding the exact evidence digests of its inputs.
  The distribution model (`uniform-scaled-inertia/1`) is recorded in the
  receipt so model changes are digest-visible.
- `PhysicalSurfaceLedger` — maps `surface::SurfaceDomainId` to consumer
  bindings across render material regions, physics surfaces, damage regions,
  audio surfaces, and weather exposure. `DamageSurfaceDomain` and
  `ShelterSurfaceDomain` resolve exposed material through
  `carto::surface::resolve_material_exposure`. Entries are keyed and ordered
  by domain identity: insertion order never changes the digest.
- `GroundPressureEvidence` — uniform contact-patch pressure derived from a
  validated mass receipt and an authored support footprint, with fixed
  standard gravity.
- `PhysicalTwinRecord` + `PhysicalTwinAcceptanceReceipt` — the twin document
  binds all of the above to an exact source revision and digest.
  `evaluate_physical_twin_acceptance` fails closed with `stale_data` when
  derived values do not bind the exact evidence digests of the twin's inputs,
  and reports conservation violations (mass/volume increase without an
  authored source change) as `accepted = false`, leaving the last-good twin
  untouched.

## Invariants

- Same inputs produce byte-identical serializations and digests.
- Every deserializer rejects future schema versions (`version_mismatch`),
  oversized counts before allocation, duplicate identities, trailing data,
  and non-finite values.
- Runtime handles, engine types, and game logic never appear in these
  records. Game-owned state is fenced out of canonical digests by
  construction, not by convention.
