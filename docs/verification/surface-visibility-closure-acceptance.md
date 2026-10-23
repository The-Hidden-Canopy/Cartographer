# Surface visibility closure acceptance

This is a bounded CPU geometric-occlusion acceptance. It is not ambient
occlusion/PBR parity, GPU timing, downstream runtime evidence, or 8K acceptance.

## Focused test

```powershell
cmake --build build/default --config Release --target cartographer_surface_visibility_closure_tests --parallel 4
ctest --test-dir build/default -R cartographer_surface_visibility_closure_tests --output-on-failure
```

The test proves deterministic source/candidate visibility distributions,
revision and QEM binding, average visibility bounds, forged receipt rejection,
invalid options, stale input rejection, and a parallel-patch occluder that
produces a visibility value below 1.0.

## Benchmark

```powershell
cmake --build build/kernel-bench --config Release --target cartographer_surface_visibility_closure_bench --parallel 4
build/kernel-bench/benchmarks/cartographer_surface_visibility_closure_bench.exe
```

The benchmark reports a 450-to-448 synthetic candidate ray-visibility timing
and residuals. The open grid fixture may legitimately report full visibility;
that is not a general scene claim.

## Deferred acceptance

Horizon/SH fitting, material/UV/semantic seam constraints, dense occlusion
corpora, radiometric/PBR parity, GPU execution, and runtime admission remain
separate gates.
