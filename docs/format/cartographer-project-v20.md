# Cartographer project format v20

Project format v20 appends `CURVE_PATH_LINEAGE` and `ROAD_SOURCE_LINEAGE`
tables after the v19 spatial-field lineage table. Each entry records the
latest source revision for an active or deleted identity. Active records must
match their source payload exactly; deleted identities retain tombstones so
undo, redo, deletion, and later recreation cannot reuse an earlier curve or
road revision. Road-source lineage is independent of the bound centerline's
curve lineage.

Both tables are mandatory in v20, bounded to the corresponding source-count
limit, and reject invalid/duplicate identities, zero revisions, missing active
records, and active-source disagreement. A road remains admissible only when
its centerline identity and revision match the active curve source.

Schema-v19 projects migrate by seeding lineage from active curve and road
records only. Older supported schemas likewise gain no invented source values;
they seed lineage only for sources that are already present. The v19 format is
documented in [`cartographer-project-v19.md`](cartographer-project-v19.md).
