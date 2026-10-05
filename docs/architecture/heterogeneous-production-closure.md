# Heterogeneous production closure

Cartographer owns authoritative authoring state; runtime products are derived
artifacts. The production closure contract makes that boundary explicit for
render meshes, material packages, residency pages, publication packages, and
evidence bundles without requiring the public repository to own VANTA's runtime.

`carto_production` provides four fail-closed invariants:

1. A cook key binds a product to the source identity, source revision, source
   digest, artifact kind, execution form, algorithm, settings digest, and
   platform profile.
2. An admitted receipt carries measured quality, confidence, cost, output size,
   and a digest over its complete canonical evidence.
3. Publication revalidates the receipt against the current source and the
   current error, confidence, and resident-memory policy.
4. The append-only ledger never falls back to an older admitted product after a
   newer failure, cancellation, or stale result for the same key.

This is deliberately a contract layer, not a pretend GPU backend. GPU queues,
VANTA residency, remote workers, and private production adapters consume the
same receipt shape when those execution paths are enabled.
