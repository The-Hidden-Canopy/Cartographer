# Project package boundary

The cross-repository architecture specification proposes a directory package
with a human-readable manifest, an authoring database, immutable blobs, and
disposable caches:

```text
Project.carto/
  manifest.json
  document.db
  blobs/sha256/
  assets/
  previews/
  exports/
  autosave/
  recovery/
  cache/
  logs/
```

The current repository still treats the versioned text `.carto` file as the
authoritative versioned save format. Current saves use [project format
v32](../format/cartographer-project-v32.md), which retains the bounded
project-bound topology receipt ledger, scientific authoring substrate,
validated per-mesh attribute/UV payloads, provider-neutral world model,
material/texture catalog, stable project source namespace, digest-bound
engineering and CONDITION source-authoring records, authored camera/light
presentation, revisioned curve/road sources, bounded spatial-field sources,
the revisioned rig-module library and decal placement table, project authoring
standards, and monotonic
source identity lineages including deletion tombstones where deletion is
supported. Decal placements are project-persistent source bytes validated by
the production decal codec. The native Model workspace has a guarded placement
route: it captures a source-anchored hit from the frontmost visible mesh,
creates a revision-bound draft, and commits through the human authoring action.
Saved projections can also be shown as a bounded outline resolved from their
source face, per-face triangle ordinal, and barycentric vertices against the
digest-checked source mesh and shown only when the visible viewport geometry
matches it exactly. The preview is an outline only; material pixels are not
rendered. Live Windows click-through acceptance remains unverified.
`carto_project::ProjectPackage` now creates and
validates the human-readable manifest and package directories, while
`carto_assets` implements the portable content-addressed blob primitive that
the package uses. `ProjectPackage::create` does not create an empty or fake
`document.db`, because an empty file is not a SQLite authoring database and
would make package validity look better than it is. The explicit flat-project
migration path creates a real SQLite database and imports the validated source
snapshot.

Evaluation graph intent can now be stored as a content-addressed package blob.
`ProjectPackage::store_evaluation_graph` serializes a validated graph, publishes
the immutable bytes through `carto_assets`, and atomically updates the manifest
with the digest under the package file lock. `load_evaluation_graph` verifies
the referenced blob before deserializing it. Missing, zero, malformed, or
tampered graph references fail closed. This is package-level graph persistence;
the document can also bind the returned digest through
`ProjectTransaction::set_evaluation_graph_digest`. That optional reference is
part of the versioned `ProjectDocument` state, so the same digest is carried by
the transaction journal envelope and checkpoint snapshot. The document and
package manifest are still separate files: binding an arbitrary digest does not
prove that a package contains the blob, and a future multi-file transaction must
validate that relationship before claiming an atomic package commit.
`ProjectPackage::validate_document_binding` provides that explicit cross-file
check and fails on a missing or mismatched reference.

Derived interchange artifacts belong under `exports/` when a future package
writer is connected. The current headless CLI writes the same artifact to an
explicit caller-provided path: `export-gltf project.carto scene.gltf`. That
glTF is a bounded derived interchange profile, not authoring truth and not a
migration of the `.carto` serializer.

`SqliteAuthoringDatabase` opens a file-backed database in WAL mode and records
checksummed schema migrations. Schema v2 adds an insert-only
`project_document_snapshot` row for migration from the current flat `.carto`
format. `ProjectPackage::migrate_flat_project` validates a source with the
versioned project reader, stages a new package, stores the source bytes and
their SHA-256 digest, verifies byte-identical export, then publishes the
package with a same-filesystem directory rename. A referenced evaluation
graph is copied only after the source blob verifies against the document's
digest. The package integrity scan checks SQLite structure, migration
lineage, and snapshot digest/parse validity without repair. Tests cover fresh
schema creation, schema-v1 upgrade, interrupted initial migration recovery,
interrupted snapshot-row import recovery, exact flat-document export,
graph-bound document/blob migration, duplicate-import refusal,
malformed-source refusal, and reporting corrupted snapshot checksums without
rewriting them.
This is a migration bridge: normal authoring edits still save through the
flat `.carto` serializer, and crash recovery for future normalized authoring
updates remains open storage work. The package cache
directory is created for convenience but is optional when opening or
validating a package. Removing it does not rewrite the manifest or document,
invalidate a verified graph blob, or change the document's graph reference;
cache contents are disposable and are not authoring truth. Evaluated-payload/
project-package atomicity remains open storage work. The
selected system distribution is WinSQLite3 from
the installed Windows SDK on Windows and the host `SQLite3::SQLite3` development
package on other platforms. CMake fails configuration when the platform
provider is missing; it does not download or vendor SQLite. The Windows runtime
is supplied by the system `winsqlite3.dll`.

Until that tranche is accepted, the flat versioned `.carto` serializer remains
the only supported authoring save format. A package manifest/blob layout is
inspectable groundwork, not a migration of existing projects or a claim that
SQLite is the authoritative authoring store.
