# Surface closure and fidelity compiler

Cartographer now has a public, backend-neutral `carto_surface_core` slice for
the first surface-fidelity milestone. It treats a surface domain as the
identity anchor and treats geometry, normal statistics, visibility, and
runtime proxies as revision-bound derived representations.

The public path currently covers:

- deterministic `SurfaceDomain` identity, source-face coverage, source revision,
  semantic/material labels, and SHA-256 provenance;
- bounded differential analysis over a `CompiledMesh`: area, mean/angle-defect
  curvature summaries, edge classes, bounded thickness, and visibility closure;
- deterministic frequency-band summaries and a geometry-only proxy baseline;
- geometry-derived anisotropic NDF fitting from local triangle normals;
- a surface-closure candidate containing proxy geometry, NDF, visibility, error
  envelope, and migration provenance;
- a bounded CPU radiometric reference evaluator over deterministic view/light
  samples;
- representation admission against explicit appearance-conservation limits;
- a revision-bound representation tree and a derived provider-neutral fidelity package
- the producer-only JSON export is format 2, uses a neutral canonical-digest domain, and requires consumers to validate the exact versioned contract; format 1 is not emitted
  envelope.
- a public v1 representation-inheritance record with stable parent/child
  identities and explicit inherited/override field sets; unknown fields,
  overlapping declarations, and non-canonical serialization fail closed;
- root-to-child view-quality policy resolution for a target representation.
  The outcome digest binds the representation content digest, inheritance
  path, effective quality-policy digest, and measured error envelope;
- a separate camera-bound selector for current closure candidates. It estimates
  projected error from caller-supplied conservative depth/radius bounds, FOV,
  and viewport height, then returns a digest-bound read-only choice. It does not
  query the renderer camera, draw the selected candidate, or reserve residency;
- project-bound material policy resolution for representation source regions.
  The evaluator reads the project material catalog, verifies every referenced
  assignment and variant, and binds the result to both the catalog digest and
  the representation inheritance path. It records material identity; it does
  not render or mutate catalog data.
- stable-ID error-envelope constraints resolved through representation
  inheritance. Violated constraints remain individually identifiable in the
  deterministic outcome receipt; invalid limits and metric kinds fail closed;
- a deterministic inherited fracture partition plan that groups sorted
  source face identities into balanced shards with content-bound stable IDs;
- a read-only materializer that emits source-revision-bound mesh shards from
  that plan, preserving exact source face/edge/vertex/normal identity and a
  deterministic digest. It assigns whole faces and does not split triangles,
  generate fracture caps, or run a physical fracture solver.
- a project consumer admission gate that recomputes view, material,
  constraint, and fracture outcomes against the current public inputs before
  admitting a representation. Stale outcome digests and current policy
  failures are refused; this remains a public CPU-side contract, not an engine
  runtime presentation claim.
- explicit representation-migration receipts naming which appearance work
  moved out of geometry;
- bounded authoring contracts for material stratigraphy and depth exposure,
  causal weathering state, manufacturing history, surface contacts, semantic
  edge profiles, and material-derived geometry intent.

The evaluator is a reference semantics boundary. It is not GPU rendering
evidence, does not mutate authoring topology, and does not claim that the first
proxy is a production simplifier. `build_geometry_proxy` deliberately selects
source triangles deterministically so the baseline can be compared before a
topology-aware simplifier is admitted.

The closure path retains the aggregate normal and source-derived NDF/visibility
statistics when detail leaves geometry. The geometry-only baseline retains only
the selected proxy triangles. This makes the first comparison explicit:

```text
same source + same triangle budget
    geometry-only proxy
versus
    proxy + source-derived NDF + visibility closure + error receipt
```

The private persistent-polygon algorithms remain outside this library and
outside the public checkout. This API is intentionally limited to generic
identity, analysis, closure, provenance, and acceptance contracts.

The process contracts are deliberately not a topology mutation path. A
weathering state, contact field, edge profile, or material-geometry intent is
derived evidence that must retain the surface domain and source revision. A
future compiler may consume those contracts to emit geometry or appearance
representations, but it must publish a new representation and migration
receipt rather than changing the authored mesh implicitly.
