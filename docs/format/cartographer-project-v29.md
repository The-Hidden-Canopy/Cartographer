# Cartographer project schema v29

Schema v29 adds the mandatory `MESH_PATCHES` table after topology receipts,
including when the table is empty. Each record binds one canonical serialized
`MeshPatch` to a mesh asset, the patch-byte SHA-256 digest, and SHA-256 digests
of the exact source and result mesh states. The table is bounded by record and
aggregate-byte limits and rejects duplicate source revisions for one asset.

`ProjectDocument::apply_mesh_patch` validates the asset and expected mesh
revision, applies the patch to a copy, records both state digests, then commits
the new mesh and immutable patch record together. Checkpoint rebase preserves
the existing patch-history prefix and only admits appended patch records for
declared mesh write assets. Removing a mesh also removes its patch records.

Replay requires both the recorded source revision and complete canonical mesh
state digest to match. It then verifies that applying the patch produces the
recorded result digest. A stale or modified source, malformed patch, edited
history prefix, digest mismatch, or oversized table is refused. Undo preserves
the original record as history; redo adds a new record bound to the post-undo
revision.

MeshPatch schema v1 currently records vertex-position changes. Existing
`TOPOLOGY_RECEIPTS` continue to carry lineage for topology-changing operations;
schema v29 does not claim replayable add/delete/split/bevel topology patch
payloads. Schemas v1–v28 remain readable through their existing migration
paths, and legacy files do not synthesize mesh patch records.
