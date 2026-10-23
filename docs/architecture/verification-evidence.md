# Verification evidence suite

`carto_verification` is the honest-evidence layer for the world-compiler
pipeline. It records what the pipeline admitted, what it refused, which
regression gates passed, and what happened when hostile conditions were
injected — as deterministic, digest-bound, engine-neutral records. It never
imports game content and never touches runtime state.

## Records

- `CapabilityLedger` — append-only ledger of capability claims and explicit
  non-claims. A claim binds one named feature for one exact admitted product
  to the admission receipt digest that admitted it; unsupported capability is
  always recorded as a first-class `CapabilityNonClaim` with a bounded
  reason, never represented by silence or a bare flag.
  `record_prepared_capabilities` converts composed neutral capability
  results into truthful records: unsupported evidence becomes a non-claim,
  every other evidence class becomes a claim. Query view: the highest-
  sequence record decides; an unsubscribed feature and a superseded claim
  both read back as not currently supported.
- `RegressionGateSpec` / `RegressionGateOutcome` — deterministic gate over one
  prepared scene binding expected digest, an exact capability set, and
  expected source-to-prepared correspondence counts. Drift by even one
  result fails the gate; a gate "failure" is data, not an error, and
  `Result` failure stays reserved for invalid specs or unverifiable input.
- Failure injection — deterministic helpers that model named hostile
  conditions between planning and publishing: a fixed-content concurrent edit
  through the normal journaled transaction boundary, a prepublish source
  check that fails closed on any source revision/digest/namespace drift,
  forced stale render-mesh and lookdev products, and in-memory payload
  corruption (byte flip, zero range, truncation) that fails closed on
  out-of-range offsets. Post-publication corruption of one immutable product
  rejects path traversal and symlink indirection before any byte is written.
- `PipelineEvidenceReport` — one deterministic, digest-bound, serializable
  record assembling source identity and digest, preparation profile digest,
  capability claims and non-claims, sorted unique admission receipt digests,
  correspondence counts, and named gate outcomes. Receipt digests are
  extracted from the ledger and counts tallied from the envelope; the report
  digest is bound before the report is returned.

## What is not here

Engine or runtime execution, game content import, live telemetry, and any
claim that a passing structural gate promotes deferred features. The four
acceptance tiers (structural, reference-behavior, native-evidence, full-game)
remain strict and separate; only the structural tier is claimed here.

Verification: `cartographer_verification_capability_ledger_tests`,
`cartographer_verification_failure_injection_tests`,
`cartographer_verification_regression_gates_tests`,
`cartographer_verification_evidence_report_tests`, and
`cartographer_verification_corrupt_artifact_tests` cover claim supersede
semantics, explicit non-claims, stale-source fail-closed publication, payload
corruption rejection, gate drift detection, report determinism, and
hostile-input rejection.
