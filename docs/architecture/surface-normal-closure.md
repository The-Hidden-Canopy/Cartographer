# Surface normal closure reference

The fidelity compiler now connects a constrained QEM candidate to the public
distribution metric layer through an area-weighted triangle-normal closure
receipt.

## Contract

`evaluate_surface_normal_closure` validates the source domain and QEM candidate,
derives the source surface's area-weighted reference normal, bins source and
candidate triangle normals against that reference, and compares the resulting
equal-bin distributions. The receipt retains:

- source and candidate distribution digests;
- Jensen-Shannon/Fisher-Rao/Wasserstein/EMD/Sinkhorn metrics;
- source and candidate surface area;
- relative area error;
- candidate, domain, revision, options, and canonical receipt identity.

The result is correlated evidence about one candidate. It must not be counted
as an independent vote against the same normals or geometry samples.

## Boundary

This is an NDF precursor, not a full anisotropic NDF fit, visibility closure,
PBR evaluation, or downstream consumer admission. It is read-only and fail-closed for stale
domains, invalid QEM results, degenerate triangles, unsupported bin counts,
non-finite aggregates, mismatched distribution digests, and forged receipts.
The source reference normal is explicit and deterministic; the implementation
does not infer a hidden camera or lighting distribution.

## Next bounded layer

Add protected material/UV/hard-normal semantics and directional visibility
histograms, then evaluate correlated geometry/normal/visibility evidence on a
dense corpus before changing the public compiler's baseline path.
