# Navigation products

`carto_navgraph` compiles the provider-neutral world primitives
([world authoring](world-authoring.md)) into deterministic, digest-bound
navigation products for one scoped preparation. The records state static
authored traversability only: which regions exist, which routes and corridors
connect them, which structure nodes are traversal interfaces, and which agent
classes may use them. A downstream consumer owns the runtime navigation index built from these
products; games own the dynamic cost, occupancy, and knowledge layers.

## Products

- `SemanticAreaRegion` — compiled from `world::SemanticArea`, preserving
  stable source identity, semantic classification, hierarchical parent, and a
  conservative planar footprint plus vertical extent. `compile_region`
  requires a simple, non-self-intersecting planar boundary loop in the same
  world frame as the model.
- `SemanticRouteProduct` / `SemanticCorridorProduct` — compiled from
  `world::SemanticPath`. The route records the authored centerline exactly
  (ordered spine, monotone cumulative distances from zero, terminal area
  references retained as stable source identities); the corridor is its
  conservative swept footprint, expanding route bounds by half the authored
  width on the two plane axes.
- `StructureAccessProduct` — structure nodes whose authored semantic type
  names a known traversal interface (`door`, `portal`, `ladder`, `hatch`);
  authored vocabulary is preserved verbatim while the primitive
  classification is the neutral interpretation. Passive nodes produce no
  access records.
- `TraversalAnchor`, `OffMeshLinkCandidate`, `MediumTransition` — anchored
  traversal endpoints compiled from compatible ports and/or anchors, the
  static links spanning them, and medium-continuity records for
  land/water/vessel/building crossings. Static existence and span only:
  whether a link is currently passable is runtime/game state.
- `AgentProfile` with `AgentAreaBinding` / `AgentPathBinding`, plus
  `NavigationTileFootprint` records for spatial invalidation.
- `PreparedNavigationEnvelope` — the digest-bound unifying product binding
  source namespace, revision, and digest to every record above for one scoped
  world preparation. Scene content is referenced by identity, never embedded.

## Enforcement

The dynamic traversal-cost layer is game-owned and can never appear on these
records: compile-time fence traits assert that no public navigation record
carries a dynamic-cost, dynamic-knowledge, or runtime-handle member, and the
build fails if one grows such a member. No destination engine handle, cost
field, or mutation authority appears anywhere in the envelope.

## What is not here

Runtime navmesh generation, pathfinding execution, cost or occupancy layers,
streaming residency, and downstream consumer navigation-index admission.

Verification: `cartographer_navgraph_tests` and
`cartographer_navgraph_agent_profile_tests` cover deterministic compilation
from authored world primitives, footprint/bounds conservatism, access
classification, link and medium-transition validation, agent support checks,
envelope round-trip, and hostile-input rejection. Native-evidence and
full-game tiers are deferred to downstream consumer and game integration.
