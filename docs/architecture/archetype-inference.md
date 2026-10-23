# Archetype inference and instance identity

`carto_archetype` finds repetition economies in authored content and keeps
generated instance identities stable across cook and reimport. The product
side is neutral and digest-bound; the runtime side — spawning, residency, and
proprietary population algorithms — belong to downstream consumers and games.

## Structural identity

- `AssemblyInput` / `AssemblyNodeContent` — an observed assembly in source
  content: per-node mesh content digests (zero for pure grouping nodes),
  sorted unique material-region identifiers, and positional hierarchy links.
- `StructuralDigest::compute` canonicalizes node naming, numbering, sibling
  order, and link order, then binds mesh content, hierarchy shape (all node
  labels blanked), and material regions at once. Equal assemblies always
  produce equal digests; any difference in content, shape, or region
  assignment produces a differing digest. The three component digests are
  deterministic evidence that the aspects agree with the bound value.

## Inference

- `AssemblyObservation` — one observed assembly under a stable source
  identity (a scene node or instance-field recipe identity), never a runtime
  handle or display name.
- `InferenceOptions` requires at least two repetitions: a unique structure is
  authoring truth, not a repetition economy.
- `ArchetypeCandidate` — observations sharing one structural digest, with
  sorted unique member identities bound by a member-evidence digest.
- `InferenceReceipt` — deterministic evidence for one pass binding the exact
  source namespace, revision, digest over the full sorted observation set,
  deduplication counts, and the candidate-set digest; it also records the
  deduplicated instance count (sum of `repetition_count - 1`), the instances a
  game no longer has to plumb separately. Recomputing over the same source
  reproduces the receipt exactly.

## Persistent instance identity

- `derive_instance_id` — deterministic, content-derived non-zero identity for
  one ordinal of an instance-field population from recipe digest, field
  identity, seed, and ordinal; the same inputs derive the same identity across
  processes and platforms.
- `InstanceIdentitySidecar` — durable identity state per instance field. On
  reimport, surviving ordinals keep their recorded identities even when
  derivation inputs drift; dead identities (suppressed instances) stay dead
  forever and are never reissued to another ordinal, which is what prevents
  resurrection. This pairs with the world-model schema v2 instance-field
  materialization fields: geometry evidence is content-addressed and the
  physical-material/construction/surface references are stable identifiers,
  not runtime handles ([world authoring](world-authoring.md)).

## What is not here

Proprietary population algorithms, runtime spawning, residency management,
statistical fitting, and any claim that inference results become authoring
truth without an explicit admission path.

Verification: `cartographer_archetype_inference_tests` and
`cartographer_instance_identity_tests` cover digest canonicalization, minimum
repetition, receipt determinism, identity derivation stability, reimport
identity preservation, dead-identity non-reissue, and hostile-input rejection.
Native-evidence and full-game tiers are deferred to downstream consumer and game integration.
