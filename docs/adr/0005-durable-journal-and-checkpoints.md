# ADR-0005: Durable journal and checkpoints

Status: journal/checkpoint foundation and staged transaction integration
accepted; replay/UI/storage integration deferred.

## Decision

Use an append-only, project-local, revision-bound journal with a SHA-256 hash
chain. `ProjectTransaction` provides a durable mutation boundary by staging a
complete document, requiring a matching journal tail, appending the bounded
record, and only then swapping staged state into the live document. The
journal remains distinct from editor undo/redo and explicit save state.

## Invariants

- appends match the current tail revision and advance it;
- event IDs are unique within one journal;
- payload and record hashes are verified on read;
- a broken or truncated record fails closed;
- replay never starts from a newer-than-tail checkpoint;
- a failed replay callback does not get converted into a successful recovery.
- a malformed checkpoint file matching the checkpoint naming contract fails
  recovery instead of being silently skipped.
- a checkpoint filename must canonically encode the revision in its metadata;
- application and transaction snapshot envelopes are parsed by exact header,
  metadata, byte-count, and document-revision rules;
- partial journal append failures attempt to restore the pre-append byte
  boundary.

## Deferred

Checkpoint snapshots, a project transaction adapter, bounded snapshot-envelope
recovery, and per-journal sidecar locking are implemented as verified
foundations. Startup recovery UX, patch-specific delta replay, SQLite
transaction coordination across multiple files, and power-loss/fsync fault
injection require later acceptance.
