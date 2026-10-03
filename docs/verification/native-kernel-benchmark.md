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
- `EditableMesh::slide_vertex`
- `EditableMesh::delete_face`
- `EditableMesh::split_edge`
- `EditableMesh::dissolve_edge`

It uses valid quad grids of 1x1, 4x4, 8x8, and 16x16 faces. The internal-edge
dissolve case is omitted for the 1x1 fixture because that mesh has no internal
edge; all other kernels run at every size. Each case has a
seven-iteration warmup and a size-specific timed sample count. Fixture creation
and copy construction are outside the timed interval. Kernel-internal
allocation, topology rebuild, and validation remain inside the interval. The
slide case uses the lowest incident support edge for each fixture vertex and a
factor of `0.25`; its topology is expected to remain unchanged. Every
completed sample is validated after timing, and the process fails if a kernel
rejects its valid fixture or produces invalid topology.

This is CPU geometry evidence only. It is not Vulkan execution evidence, GPU
throughput, shader performance, AI inference performance, or proof of parity
with the separate IDA native training kernels. Those paths are not implemented
in this Cartographer checkout and are not represented by fabricated numbers.

## Native D3D12 kernel stress

On Windows, the D3D12 benchmark exercises the temporal resolve shader on the
native compute queue. It uses the installed DXC runtime, reuses the pipeline,
resources, descriptors, and command contexts, and resolves native D3D12
timestamp queries around the workload. The output separates GPU execution time
from host submission-and-fence time and compares one batched submission with
one submission per dispatch.

```powershell
cmake --preset d3d12-kernel-bench-release
cmake --build --preset d3d12-kernel-bench-release --parallel
.\build\d3d12-kernel-bench-release\benchmarks\cartographer_d3d12_kernel_bench.exe `
    --extent 2048 --iterations 32
```

`--extent` must be an 8-pixel multiple from 8 through 8192. Use a workload
large enough to leave the adapter's power-save state before treating a result
as throughput evidence. Record `nvidia-smi` power state, clocks, utilization,
and temperature alongside the CSV; a post-run sample alone does not prove the
clocks held during the workload. The benchmark is evidence for the D3D12
temporal path only. It is not a claim about Vulkan, desktop presentation,
other shader families, or the separate IDA native training kernels.
