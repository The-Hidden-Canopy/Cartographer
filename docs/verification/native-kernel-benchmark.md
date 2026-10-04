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
- `EditableMesh::bevel_edge` (bounded single-segment internal-edge chamfer)
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

## Authoring/render snapshot preparation

The application viewport now retains compiled mesh results by project
generation, source mesh identity, and source revision. Unchanged geometry is
therefore shared across snapshots; transform-only edits reuse the same derived
mesh, while topology edits, project replacement, recovery, and rollback clear
or invalidate the entry. Snapshot assembly also reuses the already-resolved
object/world-transform list instead of walking the authoritative scene twice.
This is a CPU-side derived-cache closure, not a GPU timing claim. The
application regression test proves reuse and rejects cross-project staleness.

The native desktop shell is also prepared for a three-context Vulkan frame
ring. `CARTO_PRESENT_MODE=performance` selects mailbox when available and
otherwise FIFO; `fifo`/`vsync` force FIFO, and `immediate`/`debug` selects
immediate only when the surface supports it. This source-level preparation is
not desktop launch or presentation acceptance: the current hardware run has
not executed the Vulkan path.

## Native D3D12 kernel stress

On Windows, the D3D12 benchmark exercises the temporal resolve shader on the
native compute queue. It uses the installed DXC runtime, reuses the pipeline,
resources, descriptors, and command contexts, and resolves native D3D12
timestamp queries around the workload. The output separates GPU execution time
from host submission-and-fence time and compares the legacy command stream,
which rebinds the same resources for every dispatch, with the optimized command
stream, which hoists the invariant pipeline/resource/root-constant binding out
of the dispatch loop. It also records the working color and motion formats so a
receipt cannot be misread as a full-precision or production-role run.

The CSV `kernel_id` is the native ontology identity
`cartographer.render.temporal_resolve`; `variant` is an experiment label, not a
new semantic operation. The ontology contract remains
`compressed_operand`/`compute`/`rgba16f_history`/`fp32`/D3D12, and is validated
before the workload is submitted. Timing remains local benchmark evidence; it
is not appended to the process-local kernel evidence ledger or exported as
telemetry.

The benchmark uses the production precision roles for this pass: RGBA16F for
current/history/resolved color and RG16F for motion. The RGBA32F/RG32F path
remains covered by the high-precision readback acceptance tests; it is not a
representative interactive temporal bandwidth configuration.

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

## 2026-10-04 adapter receipt

The source was configured and rebuilt from `d3d12-kernel-bench-release`, then
the benchmark was run without a CPU fallback:

```text
adapter: Quadro P5200
driver: 573.71
kernel_id,variant,threadgroup_x,threadgroup_y,extent,iterations,warmup_dispatches,submissions,pixels,cpu_total_us,cpu_us_per_dispatch,gpu_total_us,gpu_us_per_dispatch,gpu_mpix_per_s,timestamp_frequency,color_format,motion_format
cartographer.render.temporal_resolve,legacy,8,8,1024,128,32,1,134217728.000,93174.400,727.925,91783.168,717.056,1462.335,1000000000,rgba16_float,rg16_float
cartographer.render.temporal_resolve,optimized,8,8,1024,128,32,1,134217728.000,92761.900,724.702,91675.648,716.216,1464.050,1000000000,rgba16_float,rg16_float
cartographer.render.temporal_resolve,optimized_per_submit,8,8,1024,128,32,128,134217728.000,217550.300,1699.612,92066.432,719.269,1457.836,1000000000,rgba16_float,rg16_float
```

The GPU columns come from native D3D12 timestamp queries on the compute
queue. At 1024² over 128 dispatches, hoisting invariant bindings reduced host
time from `727.925` to `724.702` microseconds per dispatch (`0.44%` in this
receipt); GPU time stayed within the same measured range (`717.056` versus
`716.216` microseconds per dispatch). One-dispatch-per-submit remains much
more expensive on the host (`1699.612` microseconds per dispatch), while its
GPU timestamp remains close. This is a command-stream/launch optimization,
not a claim that the temporal arithmetic itself became faster.

An attempted 16x8 threadgroup change was measured on the same adapter at both
1024² and 8192² and was slower than the existing 8x8 group. It was rejected
and is not the production shader configuration. The measured result is more
important than the proposed change: the kernel keeps its 8x8 geometry and the
optimized path reuses invariant bindings only.

The same build passed the native D3D12 acceptance executable: resource
ownership, deferred destruction, device-loss recovery, timestamp queries,
compute states, in-flight contexts, DXIL indexed draw and texture readback,
PBR/tone-map/shadow/temporal dispatch, 8K temporal resolve plus probe readback,
hidden swapchain resize/present (10/10). This receipt does not promote the
CPU scientific-authoring kernels to GPU execution; those remain a separate
CPU benchmark and evidence stream.

The same executable was also run at the maximum supported extent with one
measured dispatch after the benchmark's fixed 16-dispatch warmup:

```text
kernel_id,variant,threadgroup_x,threadgroup_y,extent,iterations,warmup_dispatches,submissions,pixels,cpu_total_us,cpu_us_per_dispatch,gpu_total_us,gpu_us_per_dispatch,gpu_mpix_per_s,timestamp_frequency,color_format,motion_format
cartographer.render.temporal_resolve,legacy,8,8,8192,1,16,1,67108864.000,43663.100,43663.100,42567.712,42567.712,1576.520,1000000000,rgba16_float,rg16_float
cartographer.render.temporal_resolve,optimized,8,8,8192,1,16,1,67108864.000,43615.500,43615.500,42626.048,42626.048,1574.363,1000000000,rgba16_float,rg16_float
cartographer.render.temporal_resolve,optimized_per_submit,8,8,8192,1,16,1,67108864.000,43786.400,43786.400,42602.496,42602.496,1575.233,1000000000,rgba16_float,rg16_float
```

This is a temporal compute-pass timing at 8192x8192, not a complete 8K
presented-frame budget: it excludes scene submission, geometry, post effects,
desktop composition, and display scanout. The native acceptance now performs
the same 8192x8192 temporal dispatch with RGBA16F color/history, RG16F motion,
and a 1x1 RGBA32F probe readback; it verifies the center result is the expected
blend. It does not read back the full 8K surface, so it avoids turning a
correctness gate into an unbounded 512 MiB-plus host transfer. The immediate
post-run adapter sample for this receipt was
`Quadro P5200, 573.71, P5, 139 MHz graphics, 810 MHz memory, 98% utilization,
62 C`; it remains context rather than a sustained-clock claim.

## 2026-10-04 static-motion speed pass

The temporal shader now has an explicit static-motion fast path. The caller
sets `history_flags.w` only when the motion field is authoritative zero; the
shader never infers that condition from texture contents. The guarded path
avoids the RG16F motion load, rounding, and coordinate clamp for each pixel.
Dynamic-motion behavior remains the default when the flag is zero.

These serial runs used the same Quadro P5200, D3D12 timestamp frequency, 8x8
threadgroup, format pair, warmup policy, and iteration count. GPU jobs were not
overlapped.

```text
extent,iterations,motion_mode,variant,gpu_us_per_dispatch,gpu_mpix_per_s,cpu_us_per_dispatch
4096,16,dynamic,optimized,2971.284,5646.453,3004.019
4096,16,static,optimized,1960.974,8555.552,1985.969
8192,16,dynamic,optimized,9137.578,7344.273,9176.538
8192,16,static,optimized,7833.900,8566.469,7893.631
```

The static assertion reduced the measured GPU resolve time by `34.0%` at
4096² and `14.3%` at 8192². During the sustained 8192² run, live adapter
sampling observed 100% utilization, 1708 MHz graphics clock, 73.48 W, 67 C,
and 1814 MiB of memory; the post-run sample returned to 139 MHz and 0 MiB.
This is a temporal-pass optimization, not a full presented-frame or general
motion-field throughput claim. The native D3D12 acceptance suite remained
green at `21/21` after the shader change.
