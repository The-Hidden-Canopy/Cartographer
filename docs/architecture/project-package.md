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
authoritative v1 save format. `carto_project::ProjectPackage` now creates and
validates the human-readable manifest and package directories, while
`carto_assets` implements the portable content-addressed blob primitive that
the package uses. The package layer does not create an empty or fake
`document.db`, because an empty file is not a SQLite authoring database and
would make package validity look better than it is.

SQLite WAL storage, manifest migration execution, database integrity checks,
and package open/save integration are the next storage tranche. They require a
deliberately selected SQLite distribution and license/packaging decision; no
network or private repository is a hidden prerequisite for the headless build.

Until that tranche is accepted, deleting a build cache is safe, but the flat v1
serializer remains the only supported authoring save format. A package
manifest/blob layout is inspectable groundwork, not a migration of existing
projects and not a claim of SQLite integrity.
