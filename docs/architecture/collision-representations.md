# Collision representation candidates

`carto_collision` treats collision as a representation family over one
authored identity, not a separate asset religion. Every certified variant
shares one authored source identity and correspondence while a runtime
consumer selects the cheapest variant that satisfies its purpose, target
capability, and error budget. The library is the Cartographer side only:
deterministic, neutral, digest-bound candidate and receipt records.

## Records

- Representation family — `primitive`, `compound`, `convex_proxy`, and
  `triangle_mesh`, each serving per-consumer `CollisionPurpose` values
  (`gameplay_query`, `projectile`, `vehicle`, `debris`, `character`). A
  primitive is a sphere, box, capsule, or cylinder fit over a compiled
  authoring mesh; compound, convex, and triangle candidates derive from the
  same source with the same correspondence.
- `CollisionFitError` / `CollisionErrorMetrics` — measured, non-negative,
  mesh-local error terms: maximum outside distance, volume residual,
  volume-overlap residual, maximum penetration depth, one-sided Hausdorff
  distance, and clearance violation. A zero record means exact under every
  measured term. Deterministic geometric evidence helpers (signed-tetrahedron
  mesh volume, one-sided Hausdorff, analytic sphere overlap) back the
  measurements.
- `CollisionErrorThresholdPolicy` — the public admission contract, mirroring
  the production admission-policy idiom: policy internals stay opaque, the
  exact threshold contract is bound by digest. `evaluate_collision_error`
  refuses when any measured term exceeds its bound.
- `CollisionProfile` — the game-owned contract per game identity: permitted
  primitive kinds, measured-error budget, ordered and unique family
  preference, default motion kind, and target capability flags.
- `CollisionMotionPolicy` — per-node hierarchical motion bindings
  (`stationary`, `dynamic`, `kinematic`). Kinematic nodes must name a parent
  identity, carry their rest transform, and declare a runtime mapping string
  the consumer resolves; every other motion kind leaves all three unset.
  Runtime embodiment, current pose, and destruction state are fenced out, and
  the record is intentionally decoupled from the kinematic library by loose
  string identities.
- `CollisionVariantSelection` — the certified-variant receipt. Its basis
  binds exact source identity, revision, digest, algorithm, settings digest,
  target profile, admission policy, and hardware profile. Every considered
  candidate remains inspectable with a bounded reason code; a policy-rejected
  candidate can still carry valid measured output. The selected candidate is
  revalidated against the basis at publication time; a failed selection never
  certifies a fallback and never replaces an earlier admitted selection by
  itself.

## What is not here

downstream consumer collision-object creation, runtime broadphase/narrowphase execution,
physics simulation, game damage or destruction response, and per-frame variant
selection at runtime. Only structural candidate generation, measured error,
and admission receipts are claimed.

Verification: `cartographer_collision_candidate_tests` and
`cartographer_collision_policy_tests` cover deterministic fits, error metrics,
threshold admission, per-game profiles, motion policy validation, candidate
selection receipts, and stale/forged-input rejection. Native-evidence and
full-game tiers are deferred to downstream consumer and game integration.
