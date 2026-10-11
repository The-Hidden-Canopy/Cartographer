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
- evaluates the optimal quadric position, falling back to an endpoint when the
  local system is singular;
- rejects a collapse that creates a degenerate or duplicate face;
- accepts only a collapse that reaches the exact requested triangle count;
- compacts the surviving vertices and recomputes output normals;
- emits each accepted collapse, proposed position, cost, source face identity,
  source revision, and canonical digest.

The result is a candidate surface, not an authored-mesh mutation, runtime
admission, or closure certificate. It does not yet carry material, UV, hard-
normal, semantic, or process-field constraints because those source domains are
not represented in `CompiledMesh` alone.

## Failure behavior

Oversized source requests, stale domains, degenerate-only domains, target counts
that exceed the admitted source, boundary-protected targets that cannot be
reached exactly, collapse budgets, duplicate/degenerate output, non-finite
quadrics, and forged receipts fail closed. The existing triangle-subsampling
`build_geometry_proxy` remains the explicit bounded baseline until this path is
validated against closure metrics and protected authored attributes.

## Next bounded layer

Run QEM outputs through the spectrum/distribution receipts, then add protected
material/UV/semantic/hard-normal seams and NDF/visibility closure before
promoting QEM into `compile_surface_representation`.
