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
ctest --test-dir build/default -R cartographer_surface_material_policy_tests --output-on-failure
```

The focused test proves:

1. surface domains are bound to the compiled source revision and canonical
   digest;
2. analysis is deterministic and fail-closed for stale or forged identity;
3. frequency summaries and geometry proxies are bounded and deterministic;
4. a fidelity closure candidate has lower or equal bounded reference-space
   radiometric and specular error than the geometry-only proxy on the fixture;
5. the appearance admission gate rejects a forged error envelope;
6. representation parent/child links, same-domain runtime/closure/migration
   references, closure provenance, and fidelity package digests are validated;
7. package tampering is rejected before runtime consumption;
8. material stratigraphy/depth exposure, causal weathering, manufacturing
   history ordering, contact graph identity, edge profiles, material-derived
   geometry intent, and migration receipts are bounded and deterministic.
9. representation-inheritance records serialize and parse byte-deterministically;
   unknown fields, identity mismatches, overlapping inherited/override sets,
   unsupported view declarations, missing root policies, and contradictory
   overrides fail closed; accepted and rejected view outcomes bind stable
   representation, policy, ancestry, and error-envelope digests.
10. inherited and overridden representation material bindings resolve through
    the project catalog; missing assignments, missing root bindings, policy
    mismatches, and stale variants are refused; outcome digests bind the
    canonical catalog, representation, ancestry, and resolved assignments.
11. inherited constraint outcomes report stable violating IDs; unsupported
    declarations, invalid limits/metrics, duplicate IDs, and contradictory
    overrides fail closed.
12. inherited fracture policy creates balanced source-face partition plans
    with stable shard IDs across reruns; unsupported modes, repeated faces,
    and impossible shard counts fail closed. `materialize_fracture_geometry`
    turns the current plan into digest-bound deterministic mesh shards and
    verifies exact source-face coverage, source revision, and source topology
    identity. Faces are assigned whole; triangle cuts and fracture caps are not
    generated.
13. project admission recomputes all four policy outcomes and admits only the
    exact current digests; stale outcomes, changed representation content, and
    current constraint failures are refused. `tests/public_boundary_audit_tests.py`
    passes 28 tests, and the changed public files contain no private/API-layer
    imports.
14. per-camera representation selection estimates projected position error
    from explicit conservative depth/radius bounds, FOV, and viewport height,
    then chooses the lowest-triangle candidate satisfying view and appearance
    limits. Receipts bind the source/domain, representation tree, candidates,
    and camera options; candidate order is irrelevant, and stale/forged receipts
    fail closed. This is a read-only CPU selector; it does not query a renderer
    camera or allocate runtime residency.

The separate QEM acceptance in `surface-qem-acceptance.md` is a candidate
compiler rung only. It does not change this document's claim that the existing
surface closure compiler still uses the explicit bounded baseline proxy.

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

The cross-repository spectrum layer has a separate focused acceptance and
benchmark in `surface-spectrum-acceptance.md`. It is revision-bound analysis
input only and does not upgrade this document's closure or renderer evidence.

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
pixel/interaction, external runtime, or presented-frame evidence.

## Deferred evidence

The following remain intentionally separate acceptance layers:

- automatic material/semantic/hard-normal boundary classification for
  simplification (QEM protected-edge policies are caller supplied);
- native viewport/draw integration and runtime residency allocation for inherited
  representations; the CPU selector itself does not draw or reserve memory;
- dense 100k–1M triangle concrete corpus and 32×32 view/light corpus;
- actual displaced/normal/NDF/visibility render parity in downstream consumer;
- GPU visibility/radiometric evaluation;
- material stratigraphy, process weathering, manufacturing history, contact
  fields, semantic edge synthesis, and material-to-geometry generation beyond
  the public intent contracts;
- native presentation, swapchain, and full 8K presented-frame evidence.
