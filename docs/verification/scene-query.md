# SceneQuery verification contract

Run `cartographer_scene_query_tests` alongside the agent workbench, application, and UI suites.

The fixture proves:

- Identity and exact-name results carry the captured revision and do not mutate the source.
- Empty scope is rejected without consuming query budget; task scope filters scene and CONDITION results, and explicit out-of-scope IDs are rejected.
- Proximity reports world object-origin distances, orientation reports normalized world normals, and an open plane reports its four boundary edges.
- Missing CONDITION sidecars are reported as unavailable. Current sidecars resolve authored tag and familiarity facts; stale sidecars return `stale_data`.
- Query limits truncate deterministic scans and reject later queries after the count budget is exhausted.
- Invalid numeric input fails before charging the query budget.

Follow-up tests should cover bounded semantic-sidecar decoding, selection/hover hints, collection and prefab membership, viewport visibility, UV/material topology facts, and query integration with objective compilation and plan scheduling.
