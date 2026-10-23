# Cartographer perceptual representation optimization

Cartographer keeps the authored source as the only truth. A cheaper surface is
an evidence-bound representation product, not a replacement for the source.

The public representation layer now exposes a bounded evidence path:

```text
source identity
  -> residual/base record
  -> perceptual role map
  -> visual feature recall
  -> measured cost candidates
  -> non-dominated frontier
  -> budgeted selection receipt
```

`ResidualSurfaceCodecRecord` records the base digest, optional residual mode and
reconstruction error without exposing the algorithm or payload. The
`PerceptualRoleMap` identifies which regions carry silhouette, identity,
contact, interaction, deformation, shading, radiance, or microstructure value.
`VisualFeatureRecallMetrics` keeps measured recall and false suppression as
separate axes; it does not turn an unobserved feature into a pass.

`RepresentationFrontier` removes candidates that are strictly worse on every
declared quality and measured-cost axis. `MeasuredCostGovernor` then applies an
explicit perceptual budget and selects only candidates with admitted measured
evidence. Missing GPU or host timings are represented as missing values, never
as zero-cost fallbacks. Cost observations bind the source namespace, source
revision, and source digest; a revision number by itself is not reusable
evidence. Certification is required by default and a certified candidate must
carry the digest of its separate promotion receipt. Rejected candidates remain
inspectable but are never selectable, and experimental evidence cannot prune a
certified candidate from the frontier.

The contract is intentionally public and provider-neutral. Private runtime
selection policy, adaptive algorithms, telemetry, and downstream implementation
remain outside this repository. The resulting digest-bearing records are
suitable for `.carto` evidence packaging without granting runtime mutation
authority.
