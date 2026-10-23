# Surface spectrum acceptance

This is a CPU/reference acceptance for the first spectral layer of the fidelity
compiler. It is not GPU, downstream consumer, PBR, or 8K evidence.

## Focused test

```powershell
cmake --build build/default --config Release --target cartographer_surface_spectrum_tests --parallel 4
ctest --test-dir build/default -R cartographer_surface_spectrum_tests --output-on-failure
```

The test proves that:

1. a connected authored surface produces a deterministic, sorted, revision-bound
   spectrum and canonical digest;
2. modes are mass-normalized, ordered, and orthogonal;
3. signal energy and graph smoothness are finite and non-negative;
4. stale domains, forged receipts, non-finite samples, mismatched signal
   cardinality, and an explicit vertex cap fail closed;
5. invalid solver bounds and tampered mode coefficients are rejected.

## Benchmark

```powershell
cmake --build build/kernel-bench --config Release --target cartographer_surface_spectrum_bench --parallel 4
build/kernel-bench/benchmarks/cartographer_surface_spectrum_bench.exe
```

The CSV reports one bounded CPU dense-Jacobi run on a synthetic 20x20 grid.
Its elapsed time is a local reference measurement only; it must not be read as
GPU speed, renderer FPS, or an 8K acceptance result.

## Deferred acceptance

Sparse/partial eigensolvers for production-sized meshes, distribution metrics,
constrained QEM, closure compensation, dense concrete corpora, downstream consumer parity,
and GPU execution remain separate gates.
