# ADR-0003: Project package and blob boundary

Status: accepted for the blob foundation; package migration remains proposed.

## Decision

Keep the existing versioned `.carto` serializer as the v1 authoring truth while
introducing a standalone content-addressed blob store as a separate library.
Future directory packages may contain a manifest, database, blobs, recovery
data, and disposable caches, but a package is not valid merely because its
directory exists.

## Consequences

- Blob identity is SHA-256 content, not a mutable filename or caller-provided
  asset ID.
- Existing blob contents are rehashed before they are accepted.
- GPU, thumbnail, triangulation, and other rebuildable data remain outside
  authoring truth.
- SQLite and migrations must be added with an explicit dependency and fixture
  policy; a placeholder database is rejected.

## Rejected

The implementation does not replace the v1 serializer with an ad hoc JSON or
binary directory format, and it does not claim SQLite support without a
discoverable, licensed runtime.
