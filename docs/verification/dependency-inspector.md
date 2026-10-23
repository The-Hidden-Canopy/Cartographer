# Production dependency inspector

## Current capability

`ProductionCookGraph::inspect` returns registered nodes in deterministic cook
order and every declared dependency edge. It classifies each edge as a
registered node, a caller-declared external input, or a missing input. The
inspection does not infer that an unregistered key is available: callers must
provide an explicit authoritative external-input inventory. This is diagnostic
only; existing cook behavior for unregistered dependencies is unchanged.

`ProductionCookGraph::explain_invalidation` returns one row per graph node with
an invalidated flag, reason, and upstream dependency key when applicable. It
distinguishes changed nodes, direct and upstream dependency changes, global
products, spatial overlap, spatial disjointness, and descendants of filtered
spatial nodes. Existing invalidation-order APIs derive their results from the
same explanation path.

Cycle rejection now includes a deterministic dependency path in its diagnostic.
The graph remains unchanged when a node that closes a cycle is rejected. Graph
key indexing keeps traversal and dependency classification ordered and avoids
repeated linear searches through the node list.

## Acceptance evidence

- `cartographer_production_closure_tests` rebuilt successfully in 134 Ninja
  steps on 2026-10-20; the command exited 0.
- Focused CTest passed 1/1 for `cartographer_production_closure_tests`.
- `cook_graph_inspection_classifies_inputs_and_explains_invalidation` verifies
  deterministic order, registered/external/missing classification, duplicate
  and conflicting external inventory rejection, exact changed/dependent/
  unaffected explanations, and the dependency key for a direct cause.
- Existing graph tests verify deterministic ordering, cycle refusal without
  graph mutation, explicit cycle-path diagnostics, and compatibility of the
  existing invalidation API.
- Spatial tests verify overlap, direct disjoint filtering, global conservative
  invalidation, and that a global descendant remains clean when its only
  upstream spatial product was filtered as disjoint.

## Scope and limits

This deliverable inspects the exact graph supplied by its caller. It does not
claim that the application retains a production graph for later browsing or
that a desktop panel exists; neither is required by this queue entry. The API
depends on the caller to supply authoritative external-input identities;
absent entries are reported as missing rather than claimed live.

## Repair ledger

- Files changed: `libs/production/include/carto/production/dependency.hpp`,
  `libs/production/src/dependency.cpp`, `tests/production_closure_tests.cpp`,
  and this note.
- Capability gained: callers can inspect the exact cook graph, diagnose
  unresolved inputs, review deterministic cycle witnesses, and understand each
  product's invalidation disposition.
- Boundary improved: missing and authoritative external inputs are no longer
  conflated by the inspection result; spatially skipped descendants explain
  why they remain clean.
- Governance preserved: inspection is read-only; no production receipt,
  authoring document, or promotion state is fabricated or changed. Existing
  cook acceptance and invalidation semantics remain in place.
- Tests run: headless build target completed (134 steps); focused CTest passed
  1/1.
- Tests not run: full repository suite, maximum-size graph stress/performance
  test, and desktop interaction; no integrated desktop view exists yet.
- Risks: callers can misclassify an input if they supply an inaccurate
  external-input inventory; the result records the supplied classification but
  cannot independently prove that external source is live.
- Rollback path: revert the dependency header/source and production-closure
  test additions, then remove this verification note. No serialized format or
  project data changed.
- Intentionally rejected: silently treating every unregistered dependency as
  available in the inspection report, or changing cooking behavior to reject
  inputs that existing callers treat as authoritative external sources.
