# Cartographer project format v19

Project format v19 adds `SPATIAL_FIELD_LINEAGE` after the spatial-field source
table. Each record retains the latest source revision for an active or deleted
field identity. Active records must agree exactly with their field payload;
deleted identities remain as tombstones so undo/redo or later recreation cannot
reuse a stale source revision. Lineage identities and record counts are bounded,
and malformed, duplicate, missing, zero-revision, or inconsistent records fail
closed.

Schema-v18 projects migrate their active fields into matching lineage records.
Schema-v17 and earlier inputs do not gain fabricated field values or lineage.
The v18 format remains documented at
[`cartographer-project-v18.md`](cartographer-project-v18.md).
