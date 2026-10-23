# Cartographer project schema v33

Schema v33 adds three bounded, digest-bound character-production tables after
the v32 `AUTHORING_STANDARDS_HEX` record, each written even when empty:

- `POSE_LIBRARIES` / `POSE_LIBRARY` — canonical pose library bytes keyed by
  stable library identity. Every entry carries its edit revision and the
  SHA-256 digest of its exact canonical serialization; loading decodes the
  bytes through the public `carto_pose` pose library codec and requires the
  canonical round trip, so a tampered or stale payload fails closed.
- `WEIGHT_EDIT_RECEIPTS` / `WEIGHT_EDIT_RECEIPT` — the serialized
  flood/prune/mirror weight-edit receipts in commit order. Each record binds
  the SHA-256 of its exact bytes, and the bytes must decode through the
  `carto_weights` weight-edit receipt codec, which requires nonzero input and
  output binding digests inside the receipt.
- `CURVE_EDIT_RECEIPTS` / `CURVE_EDIT_RECEIPT` — the serialized keyed-curve
  edit receipts in commit order. Each record binds the SHA-256 of its exact
  bytes, and the bytes must decode through the `carto_motion` curve-edit
  receipt codec, which requires nonzero source and result curve digests
  inside the receipt.

The two receipt tables are historical ledgers: entries are appended through
the journaled project transaction boundary, are never revised in place, and
remain in the document across undo/redo of unrelated edits, matching the
existing topology-receipt conventions. Pose libraries are revision-checked
like rig-module templates and decal placements: insertion binds a new
identity at revision one, replacement requires the caller's current revision,
and unchanged bytes do not advance the project revision.

All three tables carry bounded counts and aggregate byte budgets; oversized
or malformed records are refused at admission and on load.

Schemas v1-v32 remain readable. Projects written by schema v32 or earlier
initialize all three tables as empty and never fabricate character records.
Schema v33 projects missing any of the three mandatory tables, or carrying
them at an earlier schema version, fail closed with a version mismatch.
