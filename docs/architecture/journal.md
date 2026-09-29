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

Journal reads and appends acquire a per-journal sidecar lock
(`<journal>.lock`), so the read/validate/write append transaction is serialized
across cooperating processes as well as within one process. Project saves and
package creation also acquire their own cross-process path locks while their
multi-step publication is in progress, and all temporary outputs use
per-operation names rather than a fixed destination-adjacent path. Failed
journal writes roll back to the pre-append byte boundary. Checkpoint filenames
are bound to their metadata revision. These locks do not form a power-loss/
fsync guarantee or a transaction across unrelated project, checkpoint, and
blob stores; those deployment and crash-injection gates remain deferred.

`carto_assets::BlobStore` supplies the same standalone SHA-256 primitive for
immutable content-addressed blobs. Blob paths are `sha256/aa/bb/<digest>`;
existing content is rehashed instead of being trusted by filename or size.

## Application mutation integration

The current v1 authoring serializer remains the explicit save format. The
`ApplicationSession` binds a project-local journal beside a saved project,
records a baseline snapshot at bind time, and appends accepted command, tool,
undo, redo, and CLI batch-import actions with before/after revisions. Project
open/save holds the project path lock across journal preparation and document
publication. If an append fails, the in-memory command is reversed and the
exact pre-action document snapshot is restored. The journal remains owned by
the application boundary rather than by `ProjectDocument`; the public document
API exposes no unjournaled state mutators. Trusted application commands and
`ProjectTransaction` are the only production mutation paths.

The `ProjectTransaction` boundary provides the corresponding lower-level
durable path for integrations that own a `ProjectDocument` directly. It
requires the document revision and journal tail to match, stages all supported
document mutations, includes the actor and operation in the journal payload,
and publishes the staged document only after the append succeeds. Serialization,
load, validation, and atomic save remain low-level persistence operations; they
do not provide a second authoring path.

The `CheckpointStore` persists and verifies snapshot bytes plus the journal
prefix, can identify pending entries after restart, and fails closed when a
checkpoint metadata file matching the checkpoint naming contract is malformed
instead of silently selecting an older record. Startup recovery UI, SQLite WAL
storage, and patch-specific delta replay are not implemented. The
`recover_from_journal` adapter does support the bounded snapshot-bearing
envelopes emitted by the application and project transaction paths; it returns
a validated candidate and fails closed on unknown envelopes or revision
mismatches. Application journal binding also requires the expected event type
and exact envelope shape, rather than searching for a document marker. A
journal or checkpoint therefore must not be presented as proof
that a crash-recovered document is ready for promotion without the remaining
user-visible recovery workflow.

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
