# Constrained surface QEM candidate

Cartographer now exposes a bounded, read-only constrained quadratic-error
metric (QEM) candidate compiler. It is deliberately separate from the existing
geometry-only proxy so the public baseline does not silently change before a
larger corpus and closure evaluation exist.

## Contract

`build_constrained_qem` is bound to one validated `SurfaceDomain`. It:

- builds one plane quadric per admitted source vertex;
- derives deterministic edge candidates from the current admitted topology;
- protects domain boundary edges by default;
- accepts explicit source-bound protected-edge policy records for material,
  constraint, or fracture boundaries; QEM conservatively locks both endpoint
  vertices, and the result digest retains the exact policy inputs;
- can resolve those policies from explicitly named source attribute layers:
  discrete face values protect edges where adjacent face values differ, while
  boolean/integer edge values protect edges whose value is nonzero. Layer IDs,
  policies, and resolved edges are retained in canonical option/result digests;
- evaluates the optimal quadric position, falling back to an endpoint when the
  local system is singular;
- rejects a collapse that creates a degenerate or duplicate face;
- accepts only a collapse that reaches the exact requested triangle count;
- compacts the surviving vertices and recomputes output normals;
- emits each accepted collapse, proposed position, cost, source face identity,
  source revision, and canonical digest.

The result is a candidate surface, not an authored-mesh mutation, runtime
admission, or closure certificate. Attribute classification is opt-in and
rule-driven; arbitrary layers are never guessed to be material, constraint, or
fracture data. Missing or stale source layers, unsupported types/domains,
conflicting policies for an edge, and use of attribute rules with a compiled-
mesh-only request fail closed. Discrete face types are boolean, signed/unsigned
32-bit integers, and registered string/object/vertex IDs. Edge rules accept
boolean and signed/unsigned 32-bit integer values only. This does not yet
detect UV or hard-normal seams, or infer semantics without an explicit rule.

## Failure behavior

Oversized source requests, stale domains, degenerate-only domains, unknown or
off-domain protected edges, target counts that exceed the admitted source,
boundary-protected targets that cannot be reached exactly, collapse budgets,
duplicate/degenerate output, non-finite quadrics, and forged receipts fail
closed. The existing triangle-subsampling
`build_geometry_proxy` remains the explicit bounded baseline until this path is
validated against closure metrics and protected authored attributes.

## Next bounded layer

Run QEM outputs through the spectrum/distribution receipts and establish
NDF/visibility closure before promoting QEM into `compile_surface_representation`.
The current camera-bound selector and fracture mesh-shard materializer operate
on closure candidates; an end-to-end QEM-to-selection/fracture path remains open.
Automatic seam guessing remains intentionally excluded; classification must be
explicitly bound to authored attribute-layer rules.
