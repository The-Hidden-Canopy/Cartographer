# Surface distribution acceptance

This is a CPU/reference acceptance for equal-bin fidelity metrics. It is not a
GPU benchmark, renderer-quality gate, or runtime selection decision.

## Focused test

```powershell
cmake --build build/default --config Release --target cartographer_surface_distribution_tests --parallel 4
ctest --test-dir build/default -R cartographer_surface_distribution_tests --output-on-failure
```

The test proves deterministic metric receipts, finite bounded values,
normalization, forged digest rejection, invalid/negative/non-finite/all-zero
input rejection, unequal-support rejection, and hard comparison caps.

## Benchmark

```powershell
cmake --build build/kernel-bench --config Release --target cartographer_surface_distribution_bench --parallel 4
build/kernel-bench/benchmarks/cartographer_surface_distribution_bench.exe
```

The benchmark measures one 256-bin CPU reference comparison, including bounded
Sinkhorn transport. It is a local timing sample only and does not imply GPU
speed, FPS, or 8K acceptance.

## Deferred acceptance

Unequal-support transport, multivariate distributions, metric calibration on a
dense concrete corpus, correlation-aware evidence fusion, and topology-aware
candidate generation remain separate gates.
