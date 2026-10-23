# SceneQuery

`carto_agent::SceneQuery` is a bounded, read-only vocabulary over one immutable `ProjectDocument` revision. It returns copied facts and stable IDs; it does not return project, scene, mesh, or CONDITION document references.

## Current query vocabulary

- Object IDs, exact stable names, and mesh asset IDs.
- Objects within a world-space radius of their transformed object origin. Results include the measured origin and distance.
- Faces whose computed world-space normal meets a requested alignment threshold.
- Boundary edges from validated mesh topology.
- CONDITION source assets with an exact tag and familiarity records with an optional exact semantic ID.
- CONDITION asset membership in an exact-name collection, prefab, or region kit. Collection queries include child collections with cycle-safe traversal; region-kit queries include assets referenced through member prefabs and deduplicate assets per kit. Membership facts retain the membership identity and name plus asset source lineage, review state, and protected features.

Every result carries the project revision used to answer it. CONDITION queries decode only the project’s authored sidecar. An absent sidecar produces an explicit unavailable diagnostic; malformed or stale sidecars fail instead of being presented as live.

## Scope and budgets

Agent tasks pass their objective scope into every query. Result rows outside that scope are omitted, while an explicit request for an out-of-scope object is denied. Every caller must provide a nonempty explicit scope; there is no implicit project-wide query mode.

Each `AgentWorkEnvelope` separately limits query count, objects examined, topology elements examined, semantic records examined, and cumulative result bytes. Each request also caps result rows. Exhausting scan or result limits sets `truncated`; an invalid request, stale semantic source, or exhausted query-count budget returns a diagnostic. Query execution never changes project revision, journal state, or undo history.

## Deliberate limits

The current vocabulary does not query viewport visibility, material regions, UV seams, non-manifold areas, production readiness, or arbitrary predicates. Spatial distance currently means object-origin distance, not distance to a surface or bounds. CONDITION membership lookup uses exact authored names; it does not infer semantic matches or establish production readiness. These omissions are explicit so a caller cannot mistake an unavailable model for a negative or current fact.
