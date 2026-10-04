# Surface closure acceptance

This document records the evidence boundary for the first public fidelity
compiler slice. It is a CPU/reference acceptance, not a presented-frame or GPU
throughput claim.

## Build and focused acceptance

From the repository root:

```powershell
cmake --preset default
cmake --build build/default --config Release --parallel 4
ctest --test-dir build/default -R cartographer_surface_closure_tests --output-on-failure
```

The focused test proves:

1. surface domains are bound to the compiled source revision and canonical
   digest;
2. analysis is deterministic and fail-closed for stale or forged identity;
3. frequency summaries and geometry proxies are bounded and deterministic;
4. a fidelity closure candidate has lower or equal bounded reference-space
   radiometric and specular error than the geometry-only proxy on the fixture;
5. the appearance admission gate rejects a forged error envelope;
6. representation parent/child links, runtime references, closure provenance,
   and VANTA package digests are validated;
7. package tampering is rejected before runtime consumption;
8. material stratigraphy/depth exposure, causal weathering, manufacturing
   history ordering, contact graph identity, edge profiles, material-derived
   geometry intent, and migration receipts are bounded and deterministic.

## Optional benchmark

The benchmark is opt-in because it is a CPU reference workload:

```powershell
cmake --preset kernel-bench-release
cmake --build build/kernel-bench --config Release --target cartographer_surface_closure_bench --parallel 4
build/kernel-bench/benchmarks/cartographer_surface_closure_bench.exe
```

The CSV reports compile time and error-envelope values for deterministic
geometry-only and fidelity candidates at equal triangle budgets. It does not
represent a GPU render time, an 8K frame rate, or a claim that the current
triangle selector is a production decimator.

## Native workbench connection

The optional native desktop target connects the same public compiler through
the Measure bay's Surface Fidelity instrument:

```powershell
cmake --build build/native --target cartographer_desktop --parallel 4
ctest --test-dir build/default -R cartographer_ui_tests --output-on-failure
```

The UI path requires an explicit object selection and target triangle budget,
compiles a read-only candidate, shows its admission status and error envelope,
and overlays only a candidate whose source revision, project generation, and
selected object still match. The synchronous native preview is explicitly
bounded to 4,096 source/target triangles and rejects larger requests before
compilation. A stale candidate is displayed as stale and is not projected over
the work surface. This is native-shell build and contract evidence, not
pixel/interaction, VANTA runtime, or presented-frame evidence.

## Deferred evidence

The following remain intentionally separate acceptance layers:

- topology-aware simplification with protected semantic/material/hard-normal
  boundaries;
- dense 100k–1M triangle concrete corpus and 32×32 view/light corpus;
- actual displaced/normal/NDF/visibility render parity in VANTA;
- GPU visibility/radiometric evaluation;
- material stratigraphy, process weathering, manufacturing history, contact
  fields, semantic edge synthesis, and material-to-geometry generation beyond
  the public intent contracts;
- native presentation, swapchain, and full 8K presented-frame evidence.
