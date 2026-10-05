# Surface QEM acceptance

This is a bounded CPU candidate acceptance. It is not a topology-production,
VANTA, GPU, renderer, or 8K claim.

## Focused test

```powershell
cmake --build build/default --config Release --target cartographer_surface_qem_tests --parallel 4
ctest --test-dir build/default -R cartographer_surface_qem_tests --output-on-failure
```

The test proves deterministic digest and collapse order on a small grid,
revision/domain binding, target triangle count, recomputed output validity,
forged receipt rejection, invalid options, source-target mismatch, and a
boundary-protected request that cannot be reached exactly.

## Benchmark

```powershell
cmake --build build/kernel-bench --config Release --target cartographer_surface_qem_bench --parallel 4
build/kernel-bench/benchmarks/cartographer_surface_qem_bench.exe
```

The current synthetic 450-to-448 triangle run is a CPU reference timing only.
It does not establish production simplifier throughput or visual fidelity.

## Deferred acceptance

Protected material/UV/semantic/hard-normal constraints, larger meshes, dense
concrete corpus quality, spectrum/distribution correlation, NDF/visibility
closure, and integration into the existing surface compiler remain separate
gates.
