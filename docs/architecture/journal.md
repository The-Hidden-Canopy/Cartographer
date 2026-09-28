# Durable journal boundary

`carto_journal` is a project-local recovery primitive. It is separate from
editor undo/redo and from the explicit project-save state.

## Implemented foundation

The journal stores bounded, line-oriented records with:

- event ID;
- revision before and after;
- event type;
- serialized payload;
- payload SHA-256 digest;
- commit timestamp;
- previous-entry hash;
- current-entry hash.

Append requires the caller's `revision_before` to equal the current journal
tail and requires a strictly advancing revision. Opening and replaying a
journal verifies the header, field bounds, event uniqueness, revision chain,
payload digest, and hash chain before returning records. Replay starts after a
checkpoint revision and fails closed if the checkpoint is newer than the
journal.

`carto_assets::BlobStore` supplies the same standalone SHA-256 primitive for
immutable content-addressed blobs. Blob paths are `sha256/aa/bb/<digest>`;
existing content is rehashed instead of being trusted by filename or size.

## Application mutation integration

The current v1 authoring serializer remains the explicit save format. The
`ApplicationSession` binds a project-local journal beside a saved project,
records a baseline snapshot at bind time, and appends accepted command, tool,
undo, and redo actions with before/after revisions. If an append fails, the
in-memory command is reversed and the exact pre-action document snapshot is
restored. The journal remains owned by the application boundary rather than
by `ProjectDocument`, so direct document API calls outside an application
session are not durable events.

The `ProjectTransaction` boundary provides the corresponding lower-level
durable path for integrations that own a `ProjectDocument` directly. It
requires the document revision and journal tail to match, stages all supported
document mutations, includes the actor and operation in the journal payload,
and publishes the staged document only after the append succeeds. Legacy direct
mutators remain available for the v1 serializer and are not falsely described
as durable by themselves.

The `CheckpointStore` persists and verifies snapshot bytes plus the journal
prefix, can identify pending entries after restart, and fails closed when a
checkpoint metadata file matching the checkpoint naming contract is malformed
instead of silently selecting an older record. Startup recovery UI, SQLite WAL
storage, and patch-specific replay adapters are not implemented. A journal or
checkpoint therefore must not be presented as proof that a crash-recovered
document is ready for promotion.

## Recovery rule

The future integration path is:

```text
explicit project save
  + journal tail
  -> verified checkpoint
  -> bounded journal replay
  -> candidate document
  -> full project/geometry validation
  -> user-visible recovered state
```

Any invalid record stops recovery at the last coherent revision. Silent repair,
silent truncation, and flattening a failed payload into a successful edit are
not acceptable behaviors.
