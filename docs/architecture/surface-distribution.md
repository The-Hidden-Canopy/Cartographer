# Surface distribution reference

The fidelity compiler now has a bounded metric layer for comparing ordered,
equal-bin distributions such as spectral-band energy or other explicitly
aligned surface signals.

## Contract

`make_surface_distribution` accepts finite non-negative weights, requires
positive total mass, normalizes them once, and emits a SHA-256 identity for the
normalized vector. `compare_surface_distributions` requires both distributions
to be valid and to have the same number of bins. It returns:

- Jensen-Shannon divergence;
- Fisher-Rao distance;
- one-dimensional Wasserstein distance on unit-spaced normalized support;
- the corresponding equal-bin earth-mover distance;
- bounded entropic Sinkhorn transport cost.

The equal-bin rule is intentional. The public layer does not infer a
correspondence between unrelated supports or silently resample a signal.

## Boundaries

Distribution construction, metric evaluation, and normalization are read-only.
They do not accept a mesh, mutate a representation, authorize a candidate, or
publish runtime state. Inputs, solver iterations, regularization, and matrix
size are capped. Non-finite weights, negative weights, empty/all-zero vectors,
unequal supports, stale/forged digests, and oversized comparisons fail closed.

The Sinkhorn value is a regularized transport cost and is not presented as an
unregularized metric. Wasserstein and EMD are equal here because the support is
explicitly one-dimensional and equally spaced.

## Donor adaptation

This layer ports the donor information-geometry idea and the docuseries'
distribution comparison need into a dependency-free native implementation. It
does not import the donor Python service, NumPy, remote telemetry, or a model
router.

## Next bounded layer

The metric receipt can now be used as one correlated signal in removed-
information analysis. It must not be treated as an independent vote alongside
the same underlying spectral samples; the later candidate compiler must record
which metrics share a source and avoid double-counting them.
