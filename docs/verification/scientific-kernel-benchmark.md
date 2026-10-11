# Scientific CPU kernel benchmark

This receipt covers the locally verifiable morphology/genome path from the
scientific morphology/genome docseries. It is a CPU benchmark; it deliberately
does not enable Vulkan, D3D12, CUDA, or any provider/runtime integration.
Each CSV kernel ID is also validated through the native `KernelContract`
ontology as a CPU-reference, FP64, no-fallback operation. The contract gives
the ablation a stable semantic identity; the timing rows remain local
benchmark evidence and are not appended to the process-local evidence ledger
or exported as telemetry.

Command:

```powershell
cmake --preset kernel-bench-release
cmake --build --preset kernel-bench-release --target cartographer_scientific_bench -j 6
.\build\kernel-bench\benchmarks\cartographer_scientific_bench.exe
```

Fixture:

- 128 morphology structures in a parent chain
- 128 developmental instructions and evidence-bearing trace entries
- 16 developmental stages
- 256 functional genes
- 100 lineage nodes
- 211,289-byte canonical scientific model serialization
- 40 measured iterations after 5 warm-up iterations per kernel

Receipt from the local Release build on 2026-10-04:

| Kernel | Median (µs) | P95 (µs) | Mean (µs) | Checksum |
| --- | ---: | ---: | ---: | ---: |
| `cartographer.scientific.validate` | 327.450 | 333.210 | 328.438 | 45 |
| `cartographer.scientific.serialize` | 4,392.850 | 4,686.150 | 4,421.620 | 9,508,005 |
| `cartographer.scientific.deserialize` | 7,267.400 | 7,843.265 | 7,300.865 | 5,760 |
| `cartographer.scientific.developmental_timeline` | 393.400 | 486.405 | 412.135 | 720 |
| `cartographer.scientific.developmental_impact` | 319.550 | 323.895 | 321.555 | 90 |
| `cartographer.scientific.functional_genome_diff` | 2,624.350 | 2,784.680 | 2,647.810 | 90 |
| `cartographer.scientific.lineage_validate` | 57.700 | 72.050 | 59.280 | 4,500 |
| `cartographer.scientific.evolutionary_morphospace` | 418.500 | 431.550 | 422.273 | 8,955 |

The checksum is an execution-integrity guard, not a quality score. The result
does not claim provider execution, phenotype compilation, rendered output,
GPU throughput, or live telemetry. It provides a repeatable CPU baseline for
future optimization of the authoring and inspection kernels.
