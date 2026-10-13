# CONDITION source-authoring boundary

This verification note records the public Cartographer slice derived from the CONDITION source-authoring gap review.

## Public capability

Cartographer can author and persist a neutral `condition::AuthoringDocument` inside a `.carto` project. The document carries:

- reviewed source evidence and source-linked assets;
- Safe Place, familiarity, and view-condition metadata;
- projection and physical variant candidates with explicit mutation guards;
- temporal visual packages and surface-state records;
- interaction anchors, collections, prefabs, and region-kit manifests;
- deterministic serialization and SHA-256 package provenance.

The native shell exposes this through the CONDITION workspace. A fixture can be loaded through the application session, inspected as a read-only snapshot, and exported as an admitted `.carto-condition` package.

## Governance and failure behavior

- Source authoring is committed through the existing project command boundary and remains revision-bound.
- Package publication is rejected when source evidence is stale, unreviewed, invalid, or has a dependent source-revision mismatch.
- Presentation-only variants cannot declare collision, navigation, or interaction impact.
- The package stores logical source locators only; absolute paths, parent traversal, drive-qualified paths, secrets, and runtime telemetry are not accepted.
- Package admission verifies the serialized bytes and publication digest before a consumer can use the package.
- CONDITION runtime behavior, player telemetry, private donor services, and game-owned state are intentionally outside this public module.

## Persistence and compatibility

`ProjectDocument` schema 16 stores a `CONDITION_AUTHORING` sidecar record (introduced in schema 14). Schema 15 and earlier projects remain readable through the existing compatibility path; schema 14+ documents require the record, including an explicit `NONE` record for projects without CONDITION authoring.

The acceptance tests cover fixture round-trip, illegal physical/presentation mutations, stale-source rejection, `.carto` save/reopen, undo/redo, UI workspace availability, package publication, and consumer admission.
