# Surface workflow acceptance

This acceptance note covers the immediate four-system portfolio-donor tranche.
It is deterministic CPU/reference evidence, not GPU throughput or rendered
quality evidence.

## Build and tests

From the repository root:

```powershell
cmake --build build/default --target cartographer_surface_workflow_tests --parallel 4
ctest --test-dir build/default -R cartographer_surface_workflow_tests --output-on-failure
```

The workflow test covers:

1. deterministic candidate ranking and digest stability under input reordering;
2. candidate iteration caps and byte-budget stop behavior;
3. forged candidate digest rejection;
4. evidence reuse only when source content, revision, regions, and dependencies match;
5. stale latest evidence blocking fallback to an older record;
6. local face-scoped and transitive invalidation;
7. unrelated edge-scoped artifacts remaining valid;
8. invalidation cycle rejection;
9. scene-wide one-option-per-domain fidelity selection;
10. package/geometry/closure budget enforcement and forged cost rejection;
11. mixed-revision selection rejection and stale selection rejection;
12. invalid enum rejection and bounded oversized planner/selector input rejection.

## Benchmark

The opt-in benchmark is built from the Release benchmark preset:

```powershell
cmake --build build/kernel-bench --target cartographer_surface_workflow_bench --parallel 4
build/kernel-bench/benchmarks/cartographer_surface_workflow_bench.exe
```

It reports deterministic plan, evidence-query, invalidation, and budget
selection elapsed time plus content digests. These are local CPU timings for
the workflow contracts. They are not GPU timings, power measurements, frame
rates, VANTA runtime receipts, or 8K acceptance.

## Boundary evidence

No donor repository is imported or linked. Private polygon/continuity code and
production corpora remain outside the public checkout. The public boundary
audit and docseries-closure audit remain required release gates.
