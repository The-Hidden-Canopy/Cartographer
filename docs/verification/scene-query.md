# SceneQuery verification contract

Run `cartographer_scene_query_tests` alongside the agent workbench, application, and UI suites.

The fixture proves:

- Identity and exact-name results carry the captured revision and do not mutate the source.
- Empty scope is rejected without consuming query budget; task scope filters scene and CONDITION results, and explicit out-of-scope IDs are rejected.
- Proximity reports world object-origin distances, orientation reports normalized world normals, and an open plane reports its four boundary edges.
- Missing CONDITION sidecars are reported as unavailable. Current sidecars resolve authored tag and familiarity facts; stale sidecars return `stale_data`.
- The scoped CONDITION inventory reports current mesh validation, active render/lightmap UV-set presence, and explicit material-region assignment resolution. Missing UV candidates or unresolved region assignments fail closed, and stale source lineage remains separate from the live technical observations.
- Topology-budget exhaustion marks technical checks incomplete and truncates the query. Even a complete mesh/UV/material inspection never establishes overall CONDITION readiness.
- Query limits truncate deterministic scans and reject later queries after the count budget is exhausted.
- Invalid numeric input fails before charging the query budget.

Follow-up tests should cover bounded semantic-sidecar decoding, selection/hover hints, viewport visibility, source-to-geometry digest correspondence, collision and traversal facts, and query integration with objective compilation and plan scheduling.
