# Representation federation

`carto_representation` binds one authored source identity to a federation of
purpose-specific representation variants, their measured error envelopes, and
the receipts that admit them. Runtime consumers select the cheapest variant
that satisfies a stated requirement; Cartographer records the family, the
evidence, and the deterministic selection — never runtime residency.

## Records

- `AuthoredSourceIdentity` — namespace, revision, and digest bind the whole
  family. Runtime handles, paths, and display names are never identity; the
  source digest is. Variant replacement never changes the family id.
- `RepresentationVariant` — purpose (`render`, `collision`, `navigation`,
  `sensor`, `shadow`), resident-byte cost, the error envelope referenced by
  digest, and a continuity id. Continuity survives replacement: a new byte
  product binds the same continuity id so runtime consumers never lose track
  when a variant is re-cooked.
- `RepresentationErrorEnvelope` — bounded per-variant error terms (position,
  silhouette, normal, radiance). Variants reference their envelope by digest;
  selectors consume resolved values so requirement bounds are checked
  explicitly.
- `RepresentationCertification` — binds one variant to the digest of the
  selection evidence that admitted it, at one exact certified source
  revision. The certification is only meaningful while that recorded revision
  still matches the family it certifies.
- `HardwareProfileCapability` — stable profile identity, the purposes the
  profile can embody, and its resident-byte budget. No device handles and no
  policy implementation.
- Deterministic selection — `RepresentationCapabilityRequirement` states hard
  error/cost bounds; the selector picks the cheapest satisfying variant for a
  family and hardware profile or refuses explicitly. Budgets are hard bounds:
  the selector never relaxes them to fall back to a weaker representation.
  The selection receipt digest binds family digest, requirement digest,
  capability digest, and the chosen variant, so the same inputs reproduce the
  same selection.
- `RepresentationEvidenceLedger` — append-only runtime-measured evidence in
  the production receipt-ledger idiom. Query never falls back: the latest
  record for a (family, variant, profile) key supersedes earlier ones, and a
  failed or stale latest record yields nullopt even when an older admitted
  record exists.

## What is not here

Runtime residency management, GPU memory allocation, per-frame LOD switching
at runtime, VANTA admission of representations, and any claim that measured
error envelopes imply rendered parity.

Verification: `cartographer_representation_tests` and
`cartographer_representation_continuity_tests` cover family binding, variant
upsert and continuity preservation, certification staleness, hard-bound
selection, deterministic receipts, evidence-ledger supersede semantics, and
hostile-input rejection. Native-evidence and full-game tiers are deferred to
VANTA/game integration.
