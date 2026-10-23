# Surface QEM acceptance

This is a bounded CPU candidate acceptance. It is not a topology-production,
downstream consumer, GPU, renderer, or 8K claim.

## Focused test

```powershell
cmake --build build/default --config Release --target cartographer_surface_qem_tests --parallel 4
ctest --test-dir build/default -R cartographer_surface_qem_tests --output-on-failure
```

The test proves deterministic digest and collapse order on a small grid,
revision/domain binding, target triangle count, recomputed output validity,
forged receipt rejection, invalid options, source-target mismatch, and a
boundary-protected request that cannot be reached exactly. Explicit material,
constraint, and fracture edge records are source-bound, retained in the result
digest, and preserved in output topology; unknown edges, duplicate policy
records, and invalid policy values are rejected. Opt-in face/edge attribute
rules resolve deterministic boundary edges and bind the layer IDs, policies,
and effective edge set into the digest. Missing layers, wrong domains,
unsupported value types, compiled-only use, stale sources, explicit-vs-inferred
conflicts, and two inferred policies that classify the same edge differently
are rejected.

## Benchmark

```powershell
cmake --build build/kernel-bench --config Release --target cartographer_surface_qem_bench --parallel 4
build/kernel-bench/benchmarks/cartographer_surface_qem_bench.exe
```

The current synthetic 450-to-448 triangle run is a CPU reference timing only.
It does not establish production simplifier throughput or visual fidelity.

## Deferred acceptance

View-dependent selection, UV/hard-normal seam detection, larger meshes, dense concrete corpus quality,
spectrum/distribution correlation, NDF/visibility closure, and integration
into the existing surface compiler remain separate gates. Explicit protected
edge inputs do not claim that a caller's boundary classification was
independently verified; attribute-derived classification requires an explicit
layer rule and supported type/domain.
