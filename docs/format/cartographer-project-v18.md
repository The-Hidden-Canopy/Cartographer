# Cartographer project format v18

Project format v18 adds the mandatory `SPATIAL_FIELDS` table after
`ROAD_SOURCES`. Each `SPATIAL_FIELD` record contains a stable logical identity
and a canonical `CARTOGRAPHER_SCALAR_FIELD_2D` payload. The payload binds its
semantic, explicit U/V origin and cell size, dimensions, numeric range, source
revision, and row-major samples.

Field inputs are bounded to 256 by 256 samples. Coordinates, cell size, and
values are finite and range checked. Unsupported field schema versions,
duplicate or unbound identities, oversized tables, malformed samples, and
trailing data fail closed. An edit must advance the field's source revision by
exactly one. Project mutations use `ProjectTransaction`, so failed validation
does not publish a new project revision or journal event.

The source stores no GPU texture, simulation state, generated terrain, or
runtime field handle. Schema-v17 projects migrate with an empty spatial-field
table; no field values are inferred or fabricated.
