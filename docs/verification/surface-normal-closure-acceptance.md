# Surface normal closure acceptance

This is a CPU/reference closure signal for a QEM candidate. It is not a full
NDF, PBR, GPU, VANTA, or 8K acceptance.

## Focused test

```powershell
cmake --build build/default --config Release --target cartographer_surface_normal_closure_tests --parallel 4
ctest --test-dir build/default -R cartographer_surface_normal_closure_tests --output-on-failure
```

The test proves revision/domain/candidate binding, deterministic source and
candidate distributions, area/error aggregates, child metric validation,
forged receipt rejection, invalid bin/support caps, and stale-domain rejection.

## Benchmark

```powershell
cmake --build build/kernel-bench --config Release --target cartographer_surface_normal_closure_bench --parallel 4
build/kernel-bench/benchmarks/cartographer_surface_normal_closure_bench.exe
```

The current synthetic 450-to-448 candidate comparison reports CPU timing and
area/normal evidence only. A zero or small JS value on this fixture is not a
general visual-quality guarantee.

## Deferred acceptance

Anisotropic NDF fitting, visibility/horizon closure, material/UV/semantic seam
constraints, dense concrete views/lights, radiometric parity, and runtime
admission remain separate gates.
