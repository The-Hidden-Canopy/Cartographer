# Semantic extension sidecar and schema registry

`carto_semantic` is the durable home for game/domain semantic data that must
survive project save/load, preparation, migration, and reimport without ever
becoming Cartographer-owned meaning. Cartographer owns durability, identity,
and digest binding; donor schemas own payload meaning; games own runtime
state. The project format carries the sidecar as one optional digest-bound
record (see [project format v10](../format/cartographer-project-v10.md)):
`ProjectDocument` stores the serialized sidecar bytes verbatim, verifies the
recorded digest against the exact bytes, and never parses the payload, so the
project format gains no dependency on `carto_semantic` and semantic records
gain no mutation authority over the document.

## Field ownership

Ownership is declared per field and never inferred. `FieldOwnership`
separates `native_lowerable` fields, which may become neutral prepared
evidence, from `game_owned` fields, which are fenced out of Cartographer
mutation authority and must survive every migration untouched. The
`OwnershipManifest` binds that declaration to one exact authored identity:
entries are sorted by field name and unique, so the canonical digest is
unambiguous.

## Records

- `SemanticExtensionRecord` — one neutral, versioned, digest-bound extension.
  The payload is an opaque canonical string owned by the donor schema;
  Cartographer stores, digests, and binds it to sorted, unique authored
  identities (`world::AuthoringBinding`) but never parses it.
- `SchemaRegistry` — durable cross-game schema registry. Registration is the
  only admission path; unknown namespaces and versions fail closed.
  Compatibility requires both revisions registered and every game-owned
  field of the `from` revision to survive into `to` unchanged.
- `MigrationRule` / `MigrationPlan` — a validated hop between two registered
  revisions must explicitly preserve every game-owned field; renames are
  sorted and unique in both directions. Plans are composed only from
  caller-supplied rules and registered revisions; an uncovered version jump
  is unsupported and fails closed.
- `TagLoweringMap` — digest-bound authored-tag to lowered-tag map. It binds
  the exact sidecar digest it was compiled from; verifying against any other
  sidecar fails closed, and querying an unbound tag is a fail-closed lookup
  error, never a silent identity pass-through.
- Read-only query helpers (`areas_with_type`, `areas_with_tag`,
  `bindings_for`, `paths_between`) return results sorted by stable identity
  and never mutate either input.

## Invariants

- The sidecar digest binds the exact record and manifest set; it is never
  stored inside the serialized bytes and is recomputed fail-closed on reopen.
- Every deserializer rejects future schema versions, oversized counts before
  allocation, duplicate identities, unsorted canonical forms, trailing data,
  and non-finite values.
- Payload meaning, VANTA lowering of payload content, and game runtime state
  are explicitly not here; lowering maps carry only tag bindings plus
  recorded transformation evidence.

Verification: `cartographer_semantic_sidecar_tests`,
`cartographer_semantic_schema_tests`,
`cartographer_semantic_lowering_tests`,
`cartographer_semantic_migration_tests`, and
`cartographer_semantic_query_tests` cover digest determinism, registration
and compatibility gates, game-owned preservation obligations, fail-closed
lookups, and hostile-input rejection. Native lowering and game adapters are
out-of-repo and unclaimed.
