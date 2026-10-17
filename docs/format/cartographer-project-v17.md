# Cartographer project format v17

Schema v17 adds the required `ROAD_SOURCES` table after `CURVE_PATHS`.
Projects from schemas v1 through v16 remain readable. A v16 project upgrades
with an empty road-source catalog; v17 files missing the table are rejected.

Each `ROAD_SOURCE` record contains a deterministic schema-v1 source with:

- a stable road identity and monotonic source revision;
- an exact centerline identity and curve-source revision;
- bounded width/shoulder values and maximum-grade/minimum-radius policy fields;
- typed surface, functional class, access state, and drainage intent.

The project validates the centerline reference before commit and again during
load. Editing the referenced curve without advancing the road's centerline
binding makes the transaction invalid; callers must update both in the same
project transaction. The journal remains the publication boundary.

Road coverage bounds are a conservative derived envelope over the cubic
centerline control hull, expanded by half the road width plus shoulder. They
are not a generated road surface, a terrain product, or a precise swept-volume
calculation.

The record is authoring intent only. Grade and curvature values are not yet
checked against the centerline. It does not generate road meshes, modify
terrain, simulate drainage, assert CONDITION runtime state, or import a donor
runtime. Legacy projects are not assigned inferred road sources.
