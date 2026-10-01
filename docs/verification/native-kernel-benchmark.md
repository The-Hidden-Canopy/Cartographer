# Native geometry-kernel benchmark

Cartographer exposes an opt-in native benchmark for the implemented
`carto_geometry` kernels. It is deliberately separate from CTest because the
result is machine- and scheduler-dependent.

## Run

From the repository root:

```powershell
cmake --preset kernel-bench-release
cmake --build --preset kernel-bench-release --parallel
.\build\kernel-bench\benchmarks\cartographer_geometry_bench.exe
```

The executable emits CSV columns for the kernel, grid dimensions, source mesh
size, timed iterations, minimum, median, p95, mean, and a mutation checksum.

## Scope and protocol

The benchmark directly invokes these native methods:

- `EditableMesh::extrude_face`
- `EditableMesh::inset_face`
- `EditableMesh::set_vertex_position`
- `EditableMesh::delete_face`
- `EditableMesh::split_edge`

It uses valid quad grids of 1x1, 4x4, 8x8, and 16x16 faces. Each case has a
seven-iteration warmup and a size-specific timed sample count. Fixture creation
and copy construction are outside the timed interval. Kernel-internal
allocation, topology rebuild, and validation remain inside the interval. Every
completed sample is validated after timing, and the process fails if a kernel
rejects its valid fixture or produces invalid topology.

This is CPU geometry evidence only. It is not Vulkan execution evidence, GPU
throughput, shader performance, AI inference performance, or proof of parity
with the separate IDA native training kernels. Those paths are not implemented
in this Cartographer checkout and are not represented by fabricated numbers.
