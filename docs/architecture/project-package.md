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
authoritative versioned save format. Current saves use project format v7,
which includes the bounded project-bound topology receipt ledger, generic
scientific authoring substrate, validated per-mesh attribute/UV payloads, and
the provider-neutral five-primitive world model.
`carto_project::ProjectPackage` now creates and
validates the human-readable manifest and package directories, while
`carto_assets` implements the portable content-addressed blob primitive that
the package uses. The package layer does not create an empty or fake
`document.db`, because an empty file is not a SQLite authoring database and
would make package validity look better than it is.

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

SQLite WAL storage, manifest migration execution, database integrity checks,
and evaluated-payload/project-package atomicity are the next storage tranche.
They require a deliberately selected SQLite distribution and
license/packaging decision; no
network or private repository is a hidden prerequisite for the headless build.

Until that tranche is accepted, deleting a build cache is safe, but the flat v7
serializer remains the only supported authoring save format. A package
manifest/blob layout is inspectable groundwork, not a migration of existing
projects and not a claim of SQLite integrity.
