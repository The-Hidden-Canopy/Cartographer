# Heterogeneous execution economies closure ledger

This ledger records the reviewed public implementation and its remaining
external gates. It intentionally excludes private runtime architecture,
thresholds, corpora, and optimization recipes.

“Public tranche complete” means the locally actionable Cartographer contract,
producer, persistence, and adversarial tests exist. It does not promote a
private runtime integration or hardware/corpus result that was not executed.

| Program phase | Public Cartographer evidence | Remaining gate outside this public checkout |
| --- | --- | --- |
| 1. Render product | Deterministic V2 render mesh, UV seams, UV1/color/tangent data, submeshes, bounds, native upload ABI, content-addressed publication | Private runtime mesh consumer and saved-source-to-runtime parity receipt |
| 2. Universal world primitives | Schema-v7 `SemanticArea`, `SemanticPath`, `StructureGraph`, `InstanceField`, and `VariantLayer`; canonical digest; journaled admission; binding deletion guards | Runtime blueprint/materialization consumer |
| 3. Representation/truth evidence | Source/representation/spatial/context identity, quality/cost receipt, bounded candidate decision, rejection preservation, canonical policy digest | Private cross-consumer selection and truth-admission policy |
| 4. Sparse cook graph | Deterministic DAG plus typed grid/level tile ranges and overlap-filtered invalidation | Distributed worker scheduling and runtime cache residency |
| 5. Precision/page inputs | Existing runtime-page contracts, render upload ABI, and explicit precision requirement identity | Consumer-specific page compiler and measured runtime precision promotion |
| 6. Work planning | Public cook-graph dependencies and explicit threshold contracts keep declared estimates distinct from measured evidence | Measured runtime scheduler, learned prediction, and adaptive authority |
| 7. Blueprint/semantic authoring | World primitives provide stable areas, paths, structures, recipes, variants, and project revision basis | Runtime materialization and semantic LOD execution |
| 8. Material specialization | Compiled material artifact binds supported PBR constants and texture identities; unsupported bindings fail | Private material-graph lowering and consumer-specific runtime shaders beyond the reviewed public profile |
| 9. Portable geometry reuse | Existing web-geometry package, runtime pages, provenance, evaluation publication, and invalidation contracts | Cross-pass runtime scheduling and reuse policy |
| 10. Quality evidence surface | Public receipts and diagnostics preserve source, quality, cost, status, and failure evidence | Private telemetry ingestion and live runtime overlays |
| 11. Reconstructible content | Instance recipe/digest/seed/bounds/population plus sparse suppressions and revision-bound variants persist as authoring truth | Generator execution, exception reconciliation against a live world, and corpus acceptance |
| 12. Publication | Artifact store and decision receipt revalidate exact current source and public threshold policy before publish/read | Live private preview session and runtime admission |
| 13. Hardware evidence contract | GPU time requires completed device timestamps; D3D12 native acceptance supplies real device evidence for its bounded tests | Multi-device calibration corpus and private adaptive policy enablement |
| 14. Hardening | Bounded decoders, stale/cancel/tamper/trailing/count checks, project migration, fresh headless and D3D12 Release suites, installed-package consumer proof, and public-boundary audit | Broader UX, corpus, remote-worker, and long-duration runtime acceptance |

## End-to-end public fixture

The production fixture starts with a journaled `.carto` project containing an
editable mesh and semantic world binding, saves and reopens it, compiles a
deterministic render product, persists/reopens the exact decision receipt,
publishes through the digest-bound artifact store, and reads the same bytes
back. An authored mesh edit advances the project revision and digest and makes
both the prior artifact and decision stale.

Negative fixtures cover malformed and oversized persistence, unsigned-count
wraparound, hierarchy cycles, unresolved references, duplicate identities,
non-finite data, future variant revisions, dangling authoring bindings,
unbound spatial footprints, incompatible spatial frames, graph cycles,
duplicate candidate evidence, policy/source substitution, false GPU timing,
over-budget output, cancellation/failure, tamper, truncation, and trailing
bytes.

## Acceptance boundary

The public tranche is not evidence of a final photoreal frame, large-world
streaming, game-corpus parity, remote worker execution, or private runtime
admission. Those gates require the owning runtime, reviewed private adapters,
real workloads, and measured receipts. Cartographer now provides the complete
public source and evidence boundaries those consumers must use without
publishing the protected selection/certification mechanisms themselves.
