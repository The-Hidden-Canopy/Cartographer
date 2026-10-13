# Perceptual optimization verification

The representation tests cover the first public tranche of the cross-repository
perceptual optimization program.

Acceptance cases include:

- source-bound residual and role records validate and produce stable digests;
- role entries and feature observations must be sorted, bounded, and explicit;
- a frontier removes a candidate dominated by both measured cost and visual
  evidence;
- frontier digests are stable across input order and experimental evidence
  cannot remove a certified candidate;
- the governor selects a certified candidate only when its measured timings,
  residency, error envelope, feature recall, and suppression bounds all fit;
- a certified candidate without a promotion-evidence digest is rejected;
- admitted evidence without a measured device or host time is rejected;
- a GPU budget never treats a missing GPU timing as zero cost;
- frontier dominance is component-wise across GPU, host, residency, and
  bandwidth axes, and selection prioritizes the axes declared by the budget;
- cost evidence with a different source namespace or source digest is rejected;
- zero-weight role/feature evidence and rejected promotion states fail closed;
- stale evidence and source-revision drift cannot enter the frontier;
- experimental evidence is refused by the default certification gate.

Run the focused test after configuring a build:

```text
ctest --test-dir build/default -R cartographer_representation_tests --output-on-failure
```

This proves the public contract and its fail-closed boundaries. It does not
claim GPU timing, photoreal visual fidelity, or a VANTA runtime result; those
require a real renderer/device receipt and remain separate acceptance layers.
